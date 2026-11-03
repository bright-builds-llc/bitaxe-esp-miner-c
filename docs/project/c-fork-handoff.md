> Copied from [bright-builds-llc/bitaxe-esp-miner](https://github.com/bright-builds-llc/bitaxe-esp-miner)
> at commit `4422f4b089ef6e06c8be3a9a4e00f06d428cbc09`. The text is unchanged apart from this note and
> the MIT notice at the end. It is a snapshot: the owner's decisions, which it lists as open, are recorded
> in the fork section of `AGENTS.md`.

# Handoff: from the Rust firmware to a C fork of ESP-Miner

Written 2026-10-30 for the agent and owner starting a new fork of
[bitaxeorg/ESP-Miner](https://github.com/bitaxeorg/ESP-Miner). It is the bootstrap
prompt for that fork, and the public record of why this repository paused.

- **This repository:** <https://github.com/bright-builds-llc/bitaxe-esp-miner>. Every
  path below is relative to commit `c5185a7031c1fd4b057ae8aa237dc69a04202074`, the last
  firmware and parity state, unless it starts with `reference/` or `gate:`.
- **`reference/esp-miner/`:** upstream at the old pin, `c1915b0a` (v2.14.0-11). A fork has no
  such directory, but it has upstream's history, so read these with
  `git show c1915b0a:<path>`. The fork's own tree is newer, so recheck any line number there.
- **`gate:`:** the Bitcoin Work Gate repository,
  <https://github.com/bright-builds-llc/bitaxe-turnstile-system>, at the commit this
  repository pins, `b7751451cf51c787d7523dbd44076d8e5c3ea882`.

Claims carry their basis where it matters: *measured*, *read from source* or
*estimate*. Everything was true on the date above. Treat it as a map and recheck it
against the code before relying on it.

## Bootstrap prompt

You are working in a new C fork of ESP-Miner. The goal is upstream's firmware plus the
owner's extensions, chiefly interoperability with the Bitcoin Work Gate, with as small a
difference from upstream as possible. This repository's Rust code is a specification and
a source of lessons. It is not code to translate line by line.

Work through these steps in order. Each ends on a condition you can check.

1. **Orient.** Read this document and upstream's `AGENTS.md`. Done when you can state the
   reasons in [Why the move](#why-the-move) and list the owner decisions still open in
   [Decisions for the owner](#decisions-for-the-owner).
2. **Get the owner's decisions.** Ask for the open ones that block the next step. Done
   when each is answered or explicitly deferred, and recorded in the fork.
3. **Set up the fork.** Done when:
   - `origin` is the fork and an `upstream` remote points at `bitaxeorg/ESP-Miner`;
   - upstream's `AGENTS.md` is kept, with a clearly marked section added for this project;
   - that section records the sync policy, the evidence rules and the owner's hardware
     authorization. This repository's standing authorizations do not transfer, so have the
     owner state theirs afresh before any device is touched;
   - this document is copied in, with its source commit noted.
4. **Record a baseline.** Build the unmodified fork with its own toolchain for boards 205
   and 601, and flash and observe each one. Done when the fork records, per board: source
   commit, ESP-IDF version, app image size, free space in the slot, and boot and mining
   behaviour. See [Moving a device](#moving-a-device) before the first flash.
5. **Land upstream fixes first.** Take [Candidates for upstream](#candidates-for-upstream)
   one at a time. For each, confirm it on the current tree, reproduce it where it is safe to,
   and open a small pull request from a branch off upstream `master`. Done when each is
   merged, declined, or recorded as no longer applying. This is the cheapest way to shrink
   the fork, so do it before the extensions.
6. **Build the Gate integration.** Follow [Gate integration](#gate-integration). Done when
   the conformance fixtures pass in a host test, and the Gate's own web controller drives a
   real 205 through Hello, a lease, a fault window and a stop, with evidence recorded.
7. **Add the fork-only extensions** from [Fork-only extensions](#fork-only-extensions), each
   as its own task with its own evidence.
8. **Qualify.** Done when every claim the fork makes names its board, commit and evidence.
   Nothing here carries over: all of this repository's evidence is about Rust images.

## Why the move

**Validation.** *Read from source and measured.* Upstream at the pin supports 23 board
versions in 8 families with 4 ASIC types. Its v2.15.4 release ships 21 factory images. This
repository has hardware evidence on 2 boards, both single-chip. Each board needs its own
evidence (ADR-0014), and a port has to re-earn behaviour upstream already has. Bringing up
the Gamma 601 took 18 dated evidence notes, 4 test-only firmware variants and most of the
commits in the 10 days after 2026-10-21. A fork stays on upstream's code, already in use on
those boards, for every path it leaves alone.

The burden doesn't disappear. Anything the fork changes needs its own evidence. The Gate's
board set is also closed to the 205 and 601, and a further board needs a new ADR
(`gate:` ADR-0104).

**Keeping up with upstream.** v2.15.4 has 22 device-visible commits and 16 gaps against the
Rust firmware, touching 36 checklist rows
(`docs/parity/upstream-reviews/c1915b0a-to-v2.15.4.md`). In Rust each one is a re-implementation.
In a fork most arrive through a merge. Improvements can also go upstream, which a Rust port
can't offer.

**Image size.** *Measured.* In the same 4 MiB slot (4,194,304 bytes):

| Image | Bytes | Free |
| --- | --- | --- |
| Rust, Ultra 205, at `3e6ff4e9` | 4,191,664 | 2,640 |
| Rust, Gamma 601, at `3e6ff4e9` | 4,145,888 | 48,416 |
| Upstream `esp-miner.bin`, v2.15.4 | 2,700,800 | 1,493,504 |

The upstream image also holds an embedded web UI, BLE and TLS, none of which the Rust image
has. ESP-IDF 6, BLE, TLS, mDNS and an embedded UI were all still to fit.

This reason needs care, because by our estimate most of the gap isn't Rust as a language. From
the linker map of the 205 image, grouped by one classification by crate, so only the first row is
exact and a different grouping moves the others (`std`, `core`, `alloc` came to about 311 KB in a
second one):

| Part | Bytes |
| --- | --- |
| `secp256k1` precomputed tables | 1,114,112 |
| ESP-IDF C | 852,545 |
| Project crates | 831,634 |
| `std`, `core`, `alloc` | 451,906 |
| Worker control and its curve25519/ed25519 | 322,303 |
| `serde` family | 207,220 |
| Stratum V2 crypto and SHA | 122,415 |
| `secp256k1` code | 110,582 |
| Panic backtrace | 109,887 |
| esp-rs bindings | 61,146 |

The tables come from a default. `secp256k1-sys` builds with `ECMULT_WINDOW_SIZE=15` and
`ECMULT_GEN_PREC_BITS=4`, and upstream's `reference/esp-miner/components/libsecp256k1/CMakeLists.txt`
uses 4 and 2, which the crate's `lowmemory` feature also selects. That would free about
1,080,832 bytes, 72% of the 1,490,864-byte gap. This is an *estimate* from the table formulas:
it was found while writing this document and was never built or timed. Worker control, 22% of
the gap, is the extension itself, so the fork will pay for its equivalent too. The premium for
Rust itself is perhaps 0.3 to 0.7 MB, an *estimate* with no upstream map to compare against.

So size wasn't a hard wall for Rust. It's a real cost that a fork avoids and that the validation
and sync reasons outweigh. The fork should still budget: its slot is the same 4 MiB.

**What the move costs.** Pure logic in testable Rust crates gave strong host tests, and memory
safety for parsing what arrives over USB and the network. The fork has to rebuild that discipline
in C, most of all for the Gate's parser. Keep decision logic in pure functions with host tests.

## Decisions for the owner

Each has a recommendation. None is decided.

- **Fork home and name.** No fork exists under `bright-builds-llc` as of the date above.
- **Base.** Recommended: fork upstream, keep `master` tracking upstream, and build on a branch
  starting from tag `v2.15.4`, the latest release, moving by merge to later tags. Pull requests
  to upstream start from `master`.
- **Web UI.** ADR-0034 forbade bundling Angular AxeOS, and the earlier decision to embed our own UI
  follows from that. In a fork, recommended: keep AxeOS and add what the Gate
  needs, because replacing it adds a permanent difference. This reopens the owner's 2026-10-30
  answer, so ask.
- **Device limits.** The Rust firmware refuses to mine at settings outside a catalog range and
  stops at target power plus 3 W. Copied as they are, these would stop overclockers, so a fork running
  stock upstream features must choose its own limits. Recommended: apply them only while a Gate
  lease is active.
- **BLE.** The owner wants BLE setup only with more security than upstream
  (`task-secured-ble-provisioning` in `TASKS.md`). Recommended: leave BLE off in fork builds until
  that design exists.
- **Device identity.** Whether the fork reads the existing `bwg_worker` NVS keys, keeping each
  device's identity, or starts fresh. Recommended: keep the key names and formats, which costs
  little.
- **Licence wording.** See [Licensing](#licensing).

## Fork and sync policy

These keep the difference from upstream small, which is the point of the move.

- Put extensions in new components, such as `components/bwg_worker`, with narrow hooks in
  upstream files. Keep a short list of every upstream file the fork touches and why. A merge
  conflict should land only in that list.
- Merge upstream tags rather than rebasing, so history and evidence stay stable. Review each
  release before merging, in the manner of `docs/parity/upstream-reviews/c1915b0a-to-v2.15.4.md`,
  and name the checks it invalidates. That review is the best starting point for the changes
  since the pin.
- Prefer sending a change upstream to carrying it. Follow upstream's own conventions in `main/`
  and `components/`, not this repository's.
- Bazel, `just`, Cargo and the parity ledger stay here. Use upstream's build. Add only the host
  test harness the fork needs.

## Gate integration

The contract lives in the `gate:` repository, which outranks this summary.

- **Protocols:** `bwg-worker-controller/0.4`, `bwg-worker-serial/0.2`,
  `bwg-worker-possession/0.2`, `bwg-worker-deployment-trust/0.2`,
  `bwg-worker-lease-authorization/0.2`.
- **Spec:** `gate:docs/protocol/bwg-worker-serial-0.2.md` is canonical for framing. Controller
  0.4 and possession 0.2 prose exists only in the historical `bwg-worker-serial-0.1.md`, and trust
  prose in `bwg-worker-deployment-trust-0.1.md`, so take current shapes from the conformance
  `contract.schema.json` files. Writing the missing current prose is worthwhile early work.
- **Reusable tests:** `gate:conformance/` holds language-neutral fixtures and vectors for each
  protocol, including negative cases. Gate's `web/` serial controller can drive a C device. Local
  copies of some are in `crates/bitaxe-worker-control/tests/authorization/`.
- **Rust reference:** `crates/bitaxe-worker-control`, `firmware/bitaxe/src/bwg_worker_*`,
  `crates/bitaxe-runtime/src/revocation/`, `crates/bitaxe-runtime/src/local_mining/`, and
  ADR-0015, 0016, 0018 to 0024, 0033, 0035 to 0038 and 0041.

**Shape.** The Gate page talks to the device over Web Serial on the fixed ESP32-S3 USB Serial/JTAG.
The device has no network link to the Gate, and the backend signs leases. The serial link is
plaintext JSON, with SHA-256 for integrity, and pool credentials cross USB inside the signed Start.
Noise appears only on the Stratum V2 pool link, not here.

**Framing.** LF-terminated JSON with exactly `profile, kind, sessionId, sequence, payloadBytes,
payloadSha256, payload`. Kinds are `session`, `control`, `heartbeat` and `diagnostic`. A payload is
at most 65,536 bytes and a record at most 66,560. Hash the raw payload span as transmitted, as
unpadded base64url SHA-256, and never re-serialize it.

**Session.** Hello is uncredited, at most 2,048 bytes, with `sessionId:null, sequence:0` and a
32-byte `hostNonce`. The acknowledgement adds a 16-byte session ID, device nonce, serial manifest,
firmware source commit, ELF digest, `receiveWindowBytes:2048` and `receivedBytes:0`. Sequences then
start at 1 and strictly increase, and gaps are allowed. Possession uses an Ed25519 JWS over a binding
hash. An unused Start context expires at 60 s.

**Flow control and liveness.** Cumulative `receive_credit` counts raw drained bytes, including partial
records. The host keeps at most 2,048 bytes outstanding, in writes of at most 1,024, and one 2,000 ms
deadline covers each record. A `{}` heartbeat runs every 1,000 ms, the device revokes at 2,800 ms, and
shutdown begins within 3 s. Heartbeats and credits never renew a lease or grant authority. Output
priority through one writer is heartbeat, credit, control, then diagnostics.

**Work Lease.** Duration at most 60,000 ms, renewal at most 20,000 ms and below duration. Authorization
is an Ed25519 JWS of at most 512 bytes, verified strictly (`verify_strict`, plus weak and torsion key
rejection), and its sequence commits durably before success. Unknown fields are denied.

**Capability.** A signed static document names the firmware
(`firmware/bitaxe/bwg/ultra205-capability.json`, `gamma601-capability.json`). A C firmware needs a new
Update Authority signature. That key stays on the owner's machine (`gate:` ADR-0104), so the owner
does this step.

**NVS namespace `bwg_worker`:** `device_seed` (32 bytes), `lease_seq` (JSON by key ID, at most 8),
`effect_pending` (u8), `accept_budget`, `qual_attempts`, `soak_ledger2`. Ordinary updates preserve NVS.

**Handover (ADR-0038).** A Hello during local mining is refused and starts the ordered stop, so Connect
fails for up to about 2 minutes while cooling, then succeeds. Local mining stays off while the link is
active, and resumes after 15 s of quiet unless the operator paused it. After a refused Hello it holds off
for up to 5 minutes.

**Device-local limits, whatever the Gate says (ADR-0033, ADR-0037).** On the 205: input 4.5 to 5.5 V, at
most 15 W, ASIC below 75 C, a fresh nonzero fan reading, and a safe sample within 1 s. The 601 has its own
(ADR-0041). The signed hardware profile is retired; saved settings choose the profile inside the catalog
range. Stop order: stop dispatch, reduce frequency and reset nonce, hold reset low, core voltage off, ASIC
off, fan 100%, prove cooling, fan 30%. On the 205, shutdown takes at most 22,550 ms and cooling at most
120 s to 45 C. The 601's cooling proof uses regulator temperature and a time window.

**Hard fits with upstream's structure** (*read from source*):

- `main/main.c` spawns separate job, result, power and coordinator tasks, and
  `power_management_task.c` starts and stops mining from polled flags. The Rust firmware has one
  Production Mining Session as sole owner. A 3 s revocation of dispatch and submission needs a
  single choke point, which is the main design problem. Settle it first, with the fewest upstream
  edits that can make it correct.
- Upstream reads pools from `GLOBAL_STATE->SYSTEM_MODULE`. A grant's pool must stay volatile, reach
  the coordinator by a new path, and be zeroized.
- cJSON is a DOM parser. It can't retain raw payload spans, and unknown-field and duplicate-key
  rejection must be added by hand. Canonical JSON needs a new encoder.
- No `usb_serial_jtag` or Ed25519 use exists at the pin. Any C library must match the strict checks,
  which the negative fixtures exist to prove.
- 66,560-byte frames need deliberately placed buffers. 8 KiB task stacks elsewhere are no evidence of
  adequacy.

**Evidence earned, on Rust images only:**

- 205: live acceptance (`docs/parity/evidence/20260908-worker-preparation-live-acceptance.md`), Hello
  recovery (`20260911-hello-recovery-live.md`), restoration 8/8 (`20261010-bwg007-restoration-closure.md`), a 600 s lease interrupting local
  mining (`20261018-ultra205-lease-interruption-check.md`).
- 601: `20261025-gamma601-worker-boot.md`, `20261026-gamma601-lease-interruption-check.md`.
- Hardware runs were macOS and Chrome only.

## Hazards a C port shares

- **USB receive loss.** The 4,096-byte ring drops bytes silently when full (ADR-0023), so credit must be
  produced independently of Worker commands.
- **USB lifecycle.** Re-enumeration is expected and pre-enumeration bytes are lost. Bare
  `espflash monitor` resets the chip. Hosts need raw termios. Stale replies before Hello need discard rules
  (`docs/hardware/native-usb-ownership.md`).
- **One writer.** Nothing else may emit on the protocol channel. The Rust firmware sets
  `CONFIG_ESP_CONSOLE_SECONDARY_NONE`; check the fork's effective value, since upstream's logs use this USB.
- **Internal heap.** USB, Wi-Fi and forced-internal stacks share one ordered budget
  (`docs/hardware/fixed-usb-memory-profile.md`). Measure again in C.
- **Stacks.** PSRAM stacks forbid flash and cache-disabling paths. Stratum V2 Noise completion descended
  10.6 KB in Rust, so measure rather than assume.
- **Timing.** A long read on the single HTTP task made a 6.36 s WebSocket gap.
- **Core dumps.** `NO_OVERWRITE` leaves stale dumps and no room for the next one. Compare a dump's app
  digest to the running image.
- **Browser side.** A hidden or detached Gate tab loses the link.

`docs/hardware/known-issues.md`, `docs/hardware/worker-qualification-lessons.md` and the area files under
`.codex/tasks/lessons/` hold the rest. Items about Bazel, Cargo, S32C1I code generation (ADR-0039) or
source-ownership tests are Rust-only.

## Candidates for upstream

Each is *read from source* at the pin, unconfirmed on a device, and some areas may have changed by v2.15.4.
"Where" paths are in upstream at the pin. Confirm each on the current tree first. An upstream maintainer may also know a reason for the current
behaviour, so bring evidence and ask.

| Area | What the pin does | Where | Rust reference |
| --- | --- | --- | --- |
| Failed sensor reads | Returns -1, which never trips the 75 C check | `main/thermal/EMC2101.c:123` | `crates/bitaxe-api/src/observation/safety_verdict.rs` |
| Zero fan speed | `fan_rpm` reaches only statistics, BAP and the API, with no stop | `main/tasks/fan_controller_task.c` | `crates/bitaxe-runtime/src/revocation/unsafe_detail.rs` |
| Cooling gate | `Thermal_get_chip_temp` returns -1 once the ASIC is uninitialized, so the 45 C wait after an overheat never holds, even on boards whose comment calls the reading valid | `main/thermal/thermal.c:76`, `main/tasks/power_management_task.c:183` | `crates/bitaxe-runtime/src/local_mining.rs`, `crates/bitaxe-runtime/src/cooling.rs` |
| Overheat step-down | Each overheat subtracts 100 from voltage and frequency. At or below 100, each falls back on its own to a literal 1000 mV or 400 MHz, not the board's defaults. Upstream `master` also clamps voltage since #1790 | `main/tasks/power_management_task.c:212` | `crates/bitaxe-runtime/src/local_mining.rs` |
| Manual fan after recovery | Fan 100% persists. Intended upstream, so the user notices the overheat; see `upstream-candidates.md` | `main/tasks/power_management_task.c:174` | none |
| Stop order and results | Lowers frequency, then cuts voltage before holding reset and before stopping dispatch, and ignores the results | `main/tasks/power_management_task.c:41` | ADR-0038, `crates/bitaxe-runtime/src/mining_actuation.rs` |
| OTA rollback | No `esp_ota_mark_app_valid` call or rollback setting. Field devices may need a bootloader reflash to gain it, unverified | `main/`, `sdkconfig.defaults` | `firmware/bitaxe/src/boot_validation.rs` |
| `www` upload offset | Writes at `size - remaining`, so a short image lands misplaced | `main/http_server/http_server.c:1103` | `crates/bitaxe-api/src/www_update.rs` |
| `www` erase failure | `ESP_ERROR_CHECK` panics during erase | `main/http_server/http_server.c:1087` | same |

**Security-sensitive items go through upstream's private channel.** `reference/esp-miner/SECURITY.md` asks
for GitHub's "Report a Vulnerability" flow, so this public document names only topics. Audit these on the
current tree, and report privately what holds: bounds on settings accepted over HTTP; secrets in logs and
over BAP; authentication on BLE writes. The owner holds local notes from this review.

The Rust firmware's own access gate is no model. It's looser than upstream on some Origin forms and on
`/recovery` (checklist row API-004). Its real lesson is upstream's own comment: the HTTP server's socket is
IPv6, so read the peer as `sockaddr_in6`.

## Fork-only extensions

- **Gate integration**, above.
- **Local mining policy** (ADR-0038; `crates/bitaxe-runtime/src/local_mining/`): probing the pool before
  power, restart backoff from 30 s that holds after 5 failures, and fault latching. Treating a failed start
  as a fault could go upstream on its own.
- **Crash-loop guard** (`crates/bitaxe-runtime/src/boot_guard.rs`) and **panic cutoff**
  (`firmware/bitaxe/src/panic_cutoff.rs`).
- **API additions:** `pauseReason` and a `stopping` status (`crates/bitaxe-api/src/wire.rs`).
- **Diagnostics:** flash core dumps and owned-task watchdogs (ADR-0030, ADR-0031).
- **Secured BLE setup** (`task-secured-ble-provisioning` in `TASKS.md`).
- **Descriptive telemetry** (`task-telemetry-descriptive-extensions`), if still wanted.

Leave these behind, because adopting upstream replaces them: build-time board selection, the
handwritten and Solid web UIs unless the owner decides otherwise, flat pool keys, and
`startMiningOnBoot` unless upstream has it by then.

## Hardware facts

**Gamma 601 (BM1370, TPS546).**

- TPS546 stored defaults turn the output on at cold power-on, before the first OFF write. Upstream boots
  the same way (`docs/parity/evidence/20261022-gamma601-follow-ups.md`).
- `STATUS_CML` bit 7 probably comes from a too-early first read. Bit 1 latches early in boot, cause unknown, with no
  harm seen.
- The diode reads 127.75 C until BM1370 init, then 37.5 to 40 C. Use VR temperature and time for cooling while
  unpowered (ADR-0041).
- 525 MHz at 1150 mV: mean 1068 GH/s, about 21 W, regulator peak 80 C.
- The chip tripped 75 C once in a warmer room with the fan at 8000 RPM, so thermal margin is thin.

**Ultra 205 (BM1366).**

- The "chip" reading is the EMC2101 internal sensor plus 5 C, not a die reading.
- About 60 C under auto fan, 12.2 to 12.5 W, 5.13 to 5.22 V input.
- Stop: about 23 s from pause to proven 45 C, including 6 to 8 s of ramp-down. Preparation about 10 s, first
  hashrate near 14.6 s.
- `WIFI_PS_NONE` matters: 6 outages in 9.1 h without it, 1 in 10 h with it. Upstream already sets it.

**Both.**

- Identical USB descriptors, so only the base MAC tells boards apart. Check it before writing an image.
- The 601 did not enumerate on the 205's USB-C cable but did through a USB-C hub (upstream's bitaxeGamma#37).
- USB can wedge after a panic until a USB-only replug.
- `power_on` as a reset reason can mean brown-out, EN glitch or super-watchdog.

## Process rules worth keeping

- **Attempt policy** (`docs/hardware/hardware-attempt-policy.md`): closed outcomes, no blind retry, stop on a
  repeated boundary.
- **Evidence privacy** (`docs/parity/evidence-policy.md`): committed evidence holds no IPs, MACs, SSIDs,
  credentials, pool users, workers or endpoints. Test log redaction against real formats, since vendor Wi-Fi
  logs leak the SSID. Credential files stay ignored, unread and unprinted.
- **Board admission:** match a registered base-MAC digest before any ROM write, with digests private
  (ADR-0041).
- **Test variants** (ADR-0040, ADR-0041): a marker in the image and manifest, an explicit allow to flash, and
  refusal at release. Reinstall production afterward and confirm it.
- **Hardware steps:** a task contract first, `&&` between dependent steps, checked exit codes, and flash results
  kept separate from monitor proof. Direct UART and pin access need the owner's separate authorization.
- **Claims:** name board, commit and evidence, list non-claims, and get an independent review before claiming
  parity. Snapshot tests don't prove hardware behaviour.
- **Human waits** carry no deadline. Automated safety and protocol steps always do.

## Reusable assets

| Asset | Use in the fork |
| --- | --- |
| `gate:conformance/` | Direct test input |
| `docs/parity/checklist.md`, `docs/parity/reference-surface-inventory.json` | Specification for what a device user sees |
| `docs/parity/upstream-reviews/c1915b0a-to-v2.15.4.md` | Starting point for what changed since the pin |
| `docs/web-ui/axeos-feature-inventory.md` | Specification, if the UI is replaced |
| `crates/*/fixtures`, `tools/parity/fixtures` | ASIC frames, NVS schema, defaults and Stratum vectors, pinned to the old commit. `gamma-601-tps546-config.csv` deliberately differs from upstream |
| `firmware/bitaxe/static/www`, `firmware/bitaxe/web/solid` | Only if the fork replaces AxeOS. Neither has run on upstream firmware |
| `tools/flash/src/board_identity.rs` | The idea and `register-board`. The admission path depends on our manifest and ELF marker |
| `crates/bitaxe-virtual-board`, `scripts/virtual-board` | A harness only after a new bridge. Qualification there is unfinished |
| `crates/bitaxe-simulation`, `scripts/hardware-operator` | Not usable without the Rust firmware |

## Licensing

This is an engineering note, not legal advice.

- Upstream is GPL-3.0 and this repository is MIT (Bright Builds LLC), with GPL guardrails (`PROVENANCE.md`,
  ADR-0013). The fork is GPL-3.0 as a whole, and upstream's notices stay.
- Code or text moved from here should keep the MIT copyright and permission notice.
- Upstream-derived files here are marked (`license_posture`, 30 files, plus
  `crates/bitaxe-asic/src/bm1366/upstream_init_frames.rs`). Other Rust is "MIT by default; reassess", and no
  complete expression review exists.
- Distributed images need the source and notice obligations reviewed before release.

## Moving a device

The owner's 205 and 601 run Rust production builds. Ordinary updates between Rust images preserve NVS, and the
`factory`, `ota_0` and `ota_1` slots match upstream's, but the `coredump` partition doesn't (952K against 64K), and
no Rust-to-C transition has been tried. The Rust NVS schema holds all 68 upstream keys at the pin and adds
`asicfrequency`, `fanspeed`, `mineonboot` and the `bwg_worker` namespace. v2.15.4 differs more, with `pool_1` to
`pool_8`. For the first install, use a factory install, on the assumption that
settings and Wi-Fi are entered again. Back up nothing into the repository. To return to Rust, use `just package`
and a factory install from this repository.

## State of this repository at the pause

- **Parity:** 99 rows, 96 `verified`, 3 `implemented` (CFG-002 needs a 601 readback; ASIC-009 BM1368 and ASIC-010
  BM1397 have unit and golden evidence only). This is against pin `c1915b0a`, on the Ultra 205 unless a row says
  otherwise, and is not a statement about v2.15.4.
- **Not production-ready:** the README's mining qualification still holds. There is no release.
- **Open tasks in `TASKS.md`, for the fork's purposes:**
  - Worth carrying: `secured-ble-provisioning`, `telemetry-descriptive-extensions`, the 205's late reset hold from
    `upstream-v2154-asic-power-sequencing`, and the 601 thermal margin and BAP checks from
    `parity-hardware-non-claim-evidence`.
  - Done by adopting upstream, per the review document: `esp-idf-6-upgrade`, `parity-stratum-tls`,
    `upstream-v2154-api-and-stratum`, `upstream-v2154-pool-model`, `upstream-v2154-network`.
  - Moot or staying here: the rest.
- **Open finding:** the `secp256k1` table setting above, tracked in this repository's current
  `TASKS.md` as `task-secp256k1-lowmemory`.
- **Rebuilding this state** (network needed for ESP-IDF unless `.embuild` exists):

  ```bash
  git clone --recurse-submodules https://github.com/bright-builds-llc/bitaxe-esp-miner
  ```

  Then `just doctor`, `just bootstrap-esp`, `just test`, `just package`, and `just package --board 601`. Flashing also
  needs `just register-board`. Pins: ESP-IDF `v5.5.4`, Bazel 9.1.1, `rules_rust` 0.70.0, `espflash` 4.5.0.
  `rust-toolchain.toml` says `channel = "esp"`, and `espup` and `ldproxy` have no version pin, so an exact rebuild
  may need the versions installed on the original host (rustc 1.88.0-nightly `2ab28d2e7`).
- **CI:** the workflows run the managed checks, the device-session tests and redaction. None runs `just test` or
  builds firmware.

## MIT notice for this document

    MIT License

    Copyright (c) 2026 Bright Builds LLC

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
