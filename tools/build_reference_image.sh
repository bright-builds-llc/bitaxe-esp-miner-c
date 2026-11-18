#!/usr/bin/env bash
# Builds a reference image from a pushed commit, with the public trust in reference/trust, in a fresh
# detached worktree and an empty build directory, and writes what its distribution needs beside it:
# the build's dependencies.lock, SOURCE.txt and THIRD_PARTY_NOTICES.txt. A build that changes the
# tree fails, since its source would then no longer match the image. Prints no private value.
#
# Usage: tools/build_reference_image.sh [--local] <commit> <out-dir>
#   --local  allow a commit not on origin, for checking the script; SOURCE.txt then says "not published"
# Needs ESP-IDF v6.0.2's export.sh sourced and Node 22 or later on PATH.
set -euo pipefail

source_repository="https://github.com/bright-builds-llc/bitaxe-esp-miner-c"
required_idf="v6.0.2"

fail() {
  echo "build_reference_image: $*" >&2
  exit 1
}

local_only=false
if [ "${1:-}" = "--local" ]; then
  local_only=true
  shift
fi
[ $# -eq 2 ] || fail "usage: tools/build_reference_image.sh [--local] <commit> <out-dir>"

repo=$(cd "$(dirname "$0")/.." && pwd -P)
full=$(git -C "$repo" rev-parse --verify "$1^{commit}")
out=$2
[ ! -e "$out" ] || fail "$out exists; move it aside first"
mkdir -p "$(dirname "$out")"
out=$(cd "$(dirname "$out")" && pwd -P)/$(basename "$out")

published="published on origin"
if ! git -C "$repo" branch -r --contains "$full" | grep -q "origin/"; then
  $local_only || fail "commit ${full:0:7} is not on origin; push it, or pass --local to check the script"
  published="not published: built with --local from a commit not on origin"
fi

command -v idf.py >/dev/null || fail "idf.py isn't on PATH; source ESP-IDF $required_idf's export.sh"
idf_version=$(idf.py --version)
[[ "$idf_version" == *"$required_idf"* ]] || fail "ESP-IDF $required_idf required, found: $idf_version"
command -v node >/dev/null || fail "node isn't on PATH"
node_major=$(node -p 'process.versions.node.split(".")[0]')
((node_major >= 22)) || fail "Node 22 or later required, found $(node -v)"

work=$(mktemp -d "${TMPDIR:-/tmp}/bitaxe-reference-build.XXXXXX")
tree="$work/tree"
build="$work/build"
remove_work() {
  if [ -e "$tree/.git" ]; then
    git -C "$repo" worktree remove --force "$tree"
  fi
  rm -rf "$work"
}
trap remove_work EXIT

git -C "$repo" worktree add --quiet --detach "$tree" "$full"
for sub in $(git -C "$tree" config -f .gitmodules --get-regexp path | cut -d' ' -f2); do
  pinned=$(git -C "$tree" ls-tree HEAD "$sub" | cut -d' ' -f3 | cut -f1)
  git -C "$repo/$sub" archive "$pinned" | tar -x -C "$tree/$sub"
done

trust_dir="$tree/reference/trust"
for input in trust.json capability-205.json capability-601.json; do
  [ -f "$trust_dir/$input" ] || fail "commit ${full:0:7} has no reference/trust/$input"
done

log="$out.build.log"
echo "building ${full:0:7} with $idf_version; log: $log"
mkdir -p "$build"
if ! (cd "$tree" && BWG_TRUST_DIR="$trust_dir" idf.py -B "$build" -D SDKCONFIG="$build/sdkconfig" build) >"$log" 2>&1; then
  grep -E "error:|Error at|FAILED|ninja: build stopped" "$log" | cut -c1-200 | head -20 >&2 || true
  fail "the build failed; see $log"
fi

changed=$(git -C "$tree" status --porcelain)
[ -z "$changed" ] || fail "the build changed the tree, so its source wouldn't match the image:
$changed"

mkdir -p "$out/bootloader" "$out/partition_table"
cp "$build/esp-miner.bin" "$build/esp-miner.elf" "$build/sdkconfig" "$tree/dependencies.lock" "$out/"
cp "$build/bootloader/bootloader.bin" "$out/bootloader/"
cp "$build/partition_table/partition-table.bin" "$out/partition_table/"
echo "$full" >"$out/COMMIT"
echo "$idf_version" >"$out/IDF_VERSION"
echo "node $(node -v), npm $(npm -v)" >"$out/NODE_VERSION"

python3 -I - "$out/esp-miner.bin" "$full" <<'EOF'
import re, sys
data = open(sys.argv[1], "rb").read()
short = sys.argv[2][:7].encode()
versions = {m.decode() for m in re.findall(rb"v\d+\.\d+\.\d+[-\w.]*-g" + short + rb"[-\w]*", data)}
if not versions or any(v.endswith("dirty") for v in versions):
    sys.exit(f"the image's version strings don't name a clean {short.decode()}: {sorted(versions)}")
EOF

source_url="$source_repository/tree/$full"
python3 "$tree/tools/third_party_notices.py" --build-dir "$build" --web-dir "$tree/main/http_server/axe-os" \
  --commit "$full" --source-url "$source_url" --output "$out/THIRD_PARTY_NOTICES.txt"

cat >"$out/SOURCE.txt" <<EOF
Corresponding Source for this reference image

Commit:   $full ($published)
Source:   $source_url
Built with $idf_version, node $(node -v), npm $(npm -v).

To rebuild:
  git clone $source_repository.git && cd bitaxe-esp-miner-c
  git checkout $full && git submodule update --init
  cp <this directory>/dependencies.lock .    # the exact managed components this build used
  . <ESP-IDF $required_idf>/export.sh
  BWG_TRUST_DIR=\$PWD/reference/trust idf.py build

The web UI's npm packages are installed with npm ci from the commit's package-lock.json.
EOF

(cd "$out" && shasum -a 256 esp-miner.bin esp-miner.elf bootloader/bootloader.bin \
  partition_table/partition-table.bin dependencies.lock THIRD_PARTY_NOTICES.txt >SHA256SUMS)
echo "reference image for ${full:0:7} written to $out"
