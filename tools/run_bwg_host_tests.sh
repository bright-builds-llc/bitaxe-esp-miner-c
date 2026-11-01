#!/usr/bin/env bash
# Builds and runs the pure Bitcoin Work Gate protocol core on the host, plain and under sanitizers.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
worker_dir="$repo_root/components/bwg_worker"
yyjson_dir="$repo_root/components/yyjson/yyjson/src"
fixture_dir="$worker_dir/host_test/fixtures"
log_dir="$repo_root/build/bwg_host_tests"

read -r -a sodium_cflags <<<"$(pkg-config --cflags libsodium)"
read -r -a sodium_libs <<<"$(pkg-config --libs libsodium)"

warnings=(-Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
include_flags=(
    -I"$worker_dir/proto/include"
    -I"$worker_dir/host_test"
    -isystem "$yyjson_dir"
    "${sodium_cflags[@]}"
)
common_flags=(-std=c11 -g -D_GNU_SOURCE -DBWG_FIXTURE_DIR="\"$fixture_dir\"")

sources=("$worker_dir"/proto/*.c "$worker_dir"/host_test/*.c)

# yyjson is third-party, so it builds once per variant without the project's warnings.
build_and_run() {
    local variant="$1"
    shift
    local out_dir="$log_dir/$variant"
    mkdir -p "$out_dir"

    echo "== $variant: building"
    cc "${common_flags[@]}" "$@" -isystem "$yyjson_dir" -c "$yyjson_dir/yyjson.c" -o "$out_dir/yyjson.o"
    cc "${common_flags[@]}" "$@" "${warnings[@]}" "${include_flags[@]}" \
        "${sources[@]}" "$out_dir/yyjson.o" "${sodium_libs[@]}" -o "$out_dir/bwg_host_tests"

    echo "== $variant: running"
    "$out_dir/bwg_host_tests" 2>&1 | tee "$out_dir/last_run.log"
}

build_and_run plain -O1
build_and_run sanitized -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

echo "bwg host tests: plain and sanitized builds passed"
