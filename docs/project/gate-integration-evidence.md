# Gate integration evidence

Evidence for Bitcoin Work Gate parity with the Rust firmware, recorded 2026-11-01. Every claim below is about this one image on this one board, unless a line says otherwise, and none carries over from a Rust image or an earlier image of this tree.

| | |
|---|---|
| Board | Bitaxe 205 (BM1368), 485 MHz, 1200 mV, matched by base-MAC digest before each ROM write |
| Source | Uncommitted tree on `a8b57a5`. The image reports that commit, so a build from the commit holding this file reports a different commit and hashes, from the same firmware source |
| Image | `esp-miner.bin` SHA-256 `f900e43ff1fb2805ffca10a2c7dd8df0264448b8f17e797d18aa5c97bf9766e7` |
| ELF | SHA-256 `b72acedd3a5ea5eae5dcec4bfc4a71534118c984e0ab0c6a19dcbb684986954b` |
| Gate | `bitaxe-turnstile-system` at `b7751451cf51c787d7523dbd44076d8e5c3ea882` |
| Trust | A test authority, built into this image. It is not the owner's capability or keys |
| Pool | A real pool, through a lease grant, for leases. The Rust repository's Stratum V2 fixture for V2 and Noise. Their values stay out of this file |

## Checks on this source

| Check | Result |
|---|---|
| `bash tools/run_bwg_host_tests.sh`: conformance fixtures, plain and ASan/UBSan | 597 of 597 |
| `bash tools/run_qemu_tests.sh`: Unity tests in QEMU | 248 of 248 |
| Default build, no `BWG_TRUST_DIR` | Builds, 2,977,328 bytes, no proof marker |
| Proof build | Builds, 2,978,304 bytes, marker present once |
| Test image with the test authority | Builds, 2,982,480 bytes |
| `bun scripts/bright-builds-check.ts all` | 0 findings |

## The Gate's own pages, in Chrome's real WebSerial

`tools/bwg_gate/page_run.ts` drove `conformance/bwg-worker-serial-0.2/restoration.html` by CDP, with no shim. All eight scenarios ended with no first failure, and start replies took 11.0 to 11.4 s against the 30 s limit.

| Scenario | Device's reason | Notes |
|---|---|---|
| `completion` | `challenge_satisfied` | Start, renew, restore, close. The supervisor reports `unverified` by design, with cleanup confirmed |
| `pause` | `paused` | |
| `cancel` | `cancelled` | |
| `expiry` | `lease_expired` | |
| `monotonic_uncertainty` | `monotonic_reset` | Clock stimulus used |
| `disconnect` | `connectivity_lost` | The host took the board off the USB bus by re-enumeration during a lease. Chrome reported the loss, and the board stopped |
| `reboot` | `reboot` | |
| `authorization_negatives` | `connectivity_lost` | A replayed renew was rejected, and its review reads context `current`. A replayed start was rejected, and its review reads context `mismatch` |

## Acceptance, through the Gate's page in Chrome

`tools/bwg_gate/acceptance_run.ts` ran each plan with `acceptance_supervisor.ts` judging it through Rust's window judge, in order, on one board. Each budget below is a permanent charge in the board's NVS.

| Plan | Ordinal | Result |
|---|---|---|
| `attempt_normal` | 62 | Passed. 9 renewals, 178,214 ms active, 12 accepted, 0 rejected, 61 nonces, last chip reading 44 °C |
| `attempt_diagnostic` | 63, 73 | 63 was refused, see "Open defect". 73 passed: 3,085 ms active, 2 nonces. `worker-mining-progress-v2` reports filter `bm1366-ticket-256-leading-zero-40-v1`, 4 matches, 0 misses |
| `attempt_foreground` | 64 | Passed. `heartbeat_timeout`, closed 2,801 ms after the last heartbeat, stop started at 2,809 ms |
| `attempt_heartbeat` | 65, 66, 67 | Passed three times. `heartbeat_timeout`, closed at 2,800 ms each time, stop started at 2,802, 2,826 and 2,867 ms |
| `trace` | | Available. 1 event dropped on the boot that held attempts 62 to 67. The trace drops an event rather than block, and counts it. The cause of the one drop wasn't determined |
| `restart` | | Stage `complete`, acknowledgement matched, boot ordinal 45 to 46, software reset seen, identity matched, runtime ready, 0 port reopens, 7,127 ms |
| `soak` | soak 8 | Passed. Exactly 600,000 ms, 28 renewals, 33 accepted, 0 rejected, 245 nonces, 636 samples, 0 unsafe, final chip 44 °C |
| `endpoint` | | `matchesBoard` true, HTTP 200 |
| `cadence` | 68 | Attempt passed: 66,140 ms active, 0 accepted, 29 nonces. No share fell inside it. Phases below |
| `attempt_normal`, twice more | 69, 70 | Passed. 178,908 and 178,312 ms active, 14 and 13 accepted, 0 rejected, 63 and 102 nonces, last chip reading 45 and 44 °C |
| `core_dump_self_test` | | Stage `complete`, panic reset seen, identity matched, boot ordinal 46 to 47, 7,775 ms. Section below |

The attempt ledger read 8,340,000 ms at ordinal 73, and the soak ledger 4,800,000 ms at ordinal 8. Ordinals 71 and 72 are this image's V2 shares, and the earlier ordinals are this session's earlier images.

The acceptance campaign's three windows ran once, on an earlier image of this tree, and aren't repeated here: window 0 was consumed and left unverified, window 1 was refused and window 2 passed.

| Cadence phase | Passed | Intervals | Longest interval | Longest execution | CPU mismatches | Priority mismatches | Send failures |
|---|---|---|---|---|---|---|---|
| `idle` | yes | 119 | 509,936 µs | 9,835 µs | 0 | 0 | 0 |
| `usb` | yes | 119 | 546,562 µs | 46,651 µs | 0 | 0 | 0 |
| `mining` | yes | 120 | 522,178 µs | 22,507 µs | 0 | 0 | 0 |

The telemetry loop runs on CPU 0 at priority 5, as the Gate's cadence contract names. The `usb` phase ran 12 probes. All intervals fell in the expected bucket, with 0 dropped observations.

## Open defect: a refused Start

Attempt 63 was refused with `command_rejected` about 200 ms after the Gate sent Start, on an idle board at 33 °C. Its record reads `unsafe_observation`, 0 ms active, closed 1 ms after activation and with no stop stage run, and the ledger charged its 30,000 ms. It happened once in this session's 73 attempts, and attempt 73, on the same image, passed.

The likely cause, from reading the code, is a race, and it hasn't been reproduced. On a paused board the power task reads the pause and then ends any lease, and a local stop reports as `unsafe_observation`. A lease that opens between that read and that call ends at once. It errs on the safe side: no work ran and the chip stayed off. The Gate sees a failed Start and a spent allowance. This image carries the defect.

## Preservation and the Close probe

Each `worker-preservation-v2` block carries the stored `mineOnBoot` and a digest over a 17-setting allowlist under the domain `worker-public-settings-c-v1`, and `unchanged` held through every step below.

| Step | `mineOnBoot` | Settings tag |
|---|---|---|
| Start | false | `209e742f` |
| Display rotation changed | false | `84fc3a9a` |
| Rotation restored | false | `209e742f` |
| `mineOnBoot` stored on, board still paused | true | `3ff698ee` |
| `mineOnBoot` stored off | false | `209e742f` |

600 Close rounds took 141 s with 0 failures. The same probe on an image before the Close fix failed 11 of 600.

## Receipts, core dump and panic cutoff

The Gate read these as `worker_preparation_receipt` and `core_dump_store_receipt` diagnostic lines.

- **Preparation:** valid after a lease and across a restart, reading `current_step` 9, `outcome` `completed`, `failure` `none`, `interrupted` false, with boot ordinal, generation and uptime. A boot with no lease reads `unavailable`.
- **Store:** every boot reads `ready`, capacity 974,848 bytes. The boot after the self-test reads `store_returned`, requested 56,936 and prepared 56,992 bytes, every result 0, `self_test_marked` true.
- **Stored dump:** read back over the ROM and decoded against this ELF. 56,992 bytes, crashed task `bwg owner`, `abort() was called`, 69 threads. The raw dump stays in private storage, and the partition was erased afterwards.
- **Panic cutoff:** the dump's `bwg_panic_cutoff_receipt` reads `{0x50434f32, 0x402, 0x402, 0x400, 0x703, 0x53544631}`: the magic, the planned pins, the output enable register holding them, the output register with the reset pin low and the enable pin high, the authority word, and the self-test marker. The fatal path runs from IRAM before the dump is written.
- **Owner watchdog:** the owner task is subscribed to the task watchdog. 33 device logs from this image hold no `task_wdt` or "Task watchdog" line, no `usb_tx_failure` line, and no panic or abort line. The logs come over Wi-Fi from a running board, so they say nothing about the moment of a reset.

The self-test is a requested panic. No watchdog expiry or real crash was exercised through this path, except the injected resets under "Crash-loop guard", which don't reach the store.

## V2 and Noise

Each attempt ran on a fresh boot, against the Rust repository's fixture, and `tools/bwg_gate/noise_judge.mjs` or `v2_judge.mjs` joined the records through Rust's pure protocol judges.

| Attempt | Path | Result |
|---|---|---|
| Noise | Harness | Accepted, protocol judge accepted. Connect 74 ms, Act Two read 514 ms, diagnostic 1,010 ms, cleanup 2 ms |
| Noise | Chrome's WebSerial (`attempt_page.ts`) | Accepted, protocol judge accepted. Diagnostic 962 ms |
| Noise, wrong authority | Harness | Rejected: `authority_verified/authentication/wrong_authority` |
| Noise, cancelled | Harness | Cancelled: `cleanup/authority_lost/cancel_requested`, socket never created |
| V2 channel | Harness | Protocol judge accepted. 11 device events, 8 fixture events, 1 connection, longest read 512,457 µs |
| V2 channel | Chrome's WebSerial | Protocol judge accepted. Same counts, longest read 516,533 µs |
| V2 share | Chrome's WebSerial | Protocol judge accepted. 1 submitted, 1 acknowledged, longest read 513,513 µs. Heartbeat to revocation 2,800,000 µs, to shutdown 2,867,000 µs |
| V2 share | Harness | Protocol judge accepted. 2 submitted, 2 acknowledged, longest read 540,600 µs. Heartbeat to revocation 2,801,000 µs, to shutdown 2,893,000 µs |

Reconnecting after a Chrome share's stop took until attempt 7, about 23 s after heartbeats ended, because the stop outlasts the session. Rust's share judge refuses a revocation sooner than 2,800,000 µs after the last heartbeat, and both shares clear it.

## The Gate's controller, from the harness

`tools/bwg_gate/gate_host.ts` ran the Gate's controller over the same port. Timings are the device's own stamps, except the stop replies, which the host observed and which include cooling.

| Scenario | Result |
|---|---|
| `connect` | Baseline, restoration not required |
| `start_abort` (heartbeats stop during Start) | `heartbeat_timeout`, closed at 2,800 ms, stop started at 2,933 ms |
| `heartbeat_loss` (during a lease), 7 runs | `heartbeat_timeout` every time, closed at 2,800 or 2,801 ms, stop started at 2,838 to 2,894 ms. The device stopped 19,575 to 20,082 ms after the stop started |
| `limit_fault` (frequency set outside the catalog mid-lease) | `unsafe_observation` after 6,633 ms active |
| `reviews` | Clock stimulus, device reason `monotonic_reset` |
| `replay` (the same signed Start twice) | `command_rejected`; review shows signature `valid`, context `mismatch`, safe stop `none` |
| `lease_stop` | Start, renewal, `paused`, stop reply 19,768 ms |
| `expiry` | `lease_expired`, `lease_or_budget_expired`, 20,179 ms active |
| `reboot` | Next session reads `reboot` |
| `operator_pause` | A pause during a lease ends it. A paused board then took a lease, the pause returned, and a resume followed |
| `mining` | 15 renewals, 312,603 ms active, 16 accepted, 0 rejected, `paused`, stop reply 29,190 ms |

The stop's tail before cooling is about 16 to 17 s, inside the Gate's 22,550 ms bound, and the cooling proof adds at least 3 s. Rust's soak evidence records about 9.3 s for its shutdown at 575 MHz, so this stop is slower than Rust's while inside the same bound.

## A real fan stop

The fan was set to 0 through the API during a lease, with nothing injected. The lease ended with `unsafe_observation` after 10,985 ms active. The Gate's parser read `worker_revocation_detail` with trigger `zero_fan`, fact `none`, 1,025 ms since the last safe sample. The fan read 0 rpm for about 17 s of the stop, because it goes to 100% only after power is off, and the chip read 41 °C throughout. The settings were restored afterwards.

## Crash-loop guard

Three timer group watchdog resets in a row, injected over the board's built-in JTAG, each read as an interrupt watchdog reset. `mineOnBoot` was on for this test only.

- After the third, the board booted with `miningPaused` true, `pauseReason` `crash_loop`, 0 mV core voltage and no ASIC initialization.
- A lease then ran from that state, with a renewal and a stop reply in 19,469 ms, and the pause returned after the link went quiet.
- A resume restored local mining at about 507 GH/s and cleared the streak. The next watchdog reset did not pause the board.

## Restart guard

Local mining now paces its own restarts, as Rust's does: a failed run backs off from 30 s, doubling to 240 s, and the fifth failure in a row holds until an operator resumes. Ten minutes of hashing in one run clears the count. Pause, the Gate's link, settings and the network don't spend it. `components/bwg_worker/proto/bwg_restart_guard.c` holds the rule, with 15 host tests, and `main/bwg/bwg_local_guard.c` applies it.

A test pool that passes the upstream probe and fails every session ran against the board:

- Power-off to the next run took 31, 62, 122 and 241 s, read by a 3 s poll.
- The fifth failure read `pauseReason` `restart_guard`, `miningPaused` true and power off. One more connection came 12 s later, from a probe already in flight, and none followed in the rest of 150 s.
- A lease ran from the hold and stopped, and the pause returned with the same reason.
- With the real pools back, a resume cleared the reason and hashing returned about 13 s later.

A chip whose replies were cut over JTAG then gave real failed starts: retries came 30, 60, 120 and 241 s apart, and the fifth failure held with `restart_guard` at 494 s. After a reset and a resume, hashing returned in about 10 s. A lease on the silent chip was refused with `command_rejected` at 28.86 s, inside the 30 s limit, with the pause and its reason returned afterwards. That lease figure is from the previous image, `981a6404`, which differs only in the heartbeat stamp below. Twice on this image the test's debugger script ended partway, so the chip wasn't silent throughout and the run says nothing either way.

## Fixed by this evidence

- **Heartbeat stamp:** a heartbeat close could report 2,799 ms, which Rust's share judge would refuse as `v2_fault_device_deadline`. The stamp was taken after the timer restarted, and fixing that wasn't enough: 1 in about 10 still read 2,799 on the rerun, because a close through the link's poll counts from when the link saw the heartbeat. The reported stamp is now that same moment, and every close on this image reads 2,800 or 2,801 ms.
- **Frame read timing:** a V2 share attempt was refused with `v2_operation_deadline`, because the timer started before the idle wait, so a pool quiet for 14.8 s looked like a slow read. It now starts at a frame's first byte, as Rust's does. An observed frame now gives up after 10 s, and an observed Act Two read does too, where before only the end of authority stopped either.
- **Send and receive operations:** upstream sends from one task while another waits to receive, and they shared one open-operation slot, so a send closed a read in progress. Each direction now has its own. `test_sv2.c` holds the regression tests for both.
- **Close:** a Close that arrived while another record was still draining lost the host's final credit, so the host's Close timed out. Revoking an already closed epoch wiped the closing receipt, where Rust revokes only a current epoch. A record fully queued but still draining also counted as a partial line when something interrupted its wait, which blocked the final credit, and fixing that took the probe from 11 of 600 failures to 0. `test_writer_close.c` holds the regression tests.
- **Cooling proof:** the judge refused `attempt_normal` with `terminal_cooling_proof_missing`, because the proof passed on one reading of 45 °C and the sensor's whole degrees flickered back to 46 °C on the next. The proof now wants 3 readings in a row.
- **Heartbeat reason:** the judge refused `attempt_heartbeat` with `heartbeat_timeout_not_proven`, because the link's poll could see the lost heartbeat before the revocation timer and call it `link_closed`. Both now name it `heartbeat_timeout`, as Rust's deadline check does.
- **Telemetry priority:** the telemetry loop now runs where the Gate's cadence contract names, which turned all three phases from `passed: false` to `passed: true`.

An earlier version of this file said the owner forbade core dumps, that Close matched Rust, and that the telemetry priority should stay at 2. All three were wrong: the first had no source beyond that file, the second hid a bug in this tree, and the third gave up a check the Gate makes.

## Differences from Rust

By decision:

- **Pause during a lease:** ends the lease here. Rust keeps mining until the lease ends. A pause is the operator's direct request, so it stops the work.
- **Resume during a lease from a paused board:** the pause still returns afterwards, because the resume can't be told apart from the lease's own running state.

Not in this image, and required by no Gate path:

- **Cadence schema:** this image reports `worker-telemetry-cadence-v1`, and Rust reports v2. The Gate accepts both. v2 adds only `maximumLiveStagesUs` and `worstInterval`, whose 11 stages are those of Rust's telemetry loop. This loop has one stage, so they would read 0.
- **Startup diagnostics:** `allocation_failure_context`, `statistics_startup`, `storage_http_*`, `wifi_startup_failure`, `usb_memory_checkpoint`, `bwg_worker_start_failure` and non-ready `usb_startup` stages aren't emitted. Rust emits some of these on healthy boots too. Only Rust's install judges read them, and those bind Rust's source. Adding them would touch `main.c`, `filesystem.c`, `http_server.c` and Wi-Fi.
- **Mining progress counters:** `stale_completion`, `duplicate_candidates`, the core and address-interval discards, and the blocked `wrong_session`, `job_lookup`, `work_stale` and `target_mismatch` counts always read 0. The expected filter is a software expectation, not a reading of the chip's mask.
- **Regulator temperature:** a `vr_temperature` fact in `worker_revocation_detail` is dropped by the Gate's grammar, which doesn't list it. Rust's has the same label and the same drop.
- **Panic receipt:** `rust_panic_receipt` describes a Rust panic, so it has no meaning here.

By construction, each small and stated so nobody relies on it:

- **V2 record times:** the revoked, shutdown and cooled event times are when the owner task noted them. The true times are in the qualification atoms.
- **Block check:** `is_block` is masked during an attempt, since the fixture's target is easy.
- **Act One:** it's built inside the handshake after connect, where Rust builds it before connect.
- **Frame deadline:** a frame's header and payload each get 10 s here, where Rust gives the frame one 10 s.
- **Noise session loss:** maps to `session_replaced`, with no separate `heartbeat_expired`.
- **Cooling limit:** a board stuck at the limit stays in the proof and reports overdue at 120 s, where Rust fails at 120 s. It stays stopped either way.

Local mining policy, where it differs from Rust's:

- **What counts:** only a pool failure ending a run and a failed start count. The owner limited device limits to a lease, so local mining has no unsafe stop to count.
- **Probe order:** the first run after a boot or a resume powers the chip before probing, as upstream does. Rust probes first. Later runs probe first in both.
- **Fault latch:** upstream's `hardware_fault` lasts until a reboot and also refuses leases, where Rust's latch covers local mining only. This is stricter.
- **Overheat recovery:** upstream's, which isn't limited to the catalog. Rust's stays inside it. Device limits apply only during a lease here.

## Non-claims

- **Hardware not run:** the Gamma 601 wasn't attached, so nothing here covers it. Its board has no enable pin, so the panic cutoff there can only cut reset. A pulled cable, a power loss, a real overheat, a stalled pool send and a lost pool during a lease were not exercised. USB re-enumeration leaves the chip powered, so it stands in for a pulled cable from the board's side only.
- **Trust:** this image holds a test authority. Calling it reference firmware under the owner's signed capability is the owner's decision.
- **Judges:** Rust's full operator and install judges bind Rust's source, and nothing C-aware replaces them yet, so `noise_judge.mjs` and `v2_judge.mjs` use only its pure protocol judges and prove less. Rust's share judge predates ADR-0037's amendment, so `v2_judge.mjs` shows it the reported headroom minus the old 15,550 ms reserve.
- **Restart guard:** its count lives in RAM, so a reboot starts over. Its hashing clock can include time spent cooling after an overheat. After a backoff ends, the next run can wait up to one more probe interval.
- **Duration:** the longest lease here is the 600 s soak. Nothing supports a claim about unbounded mining.
- **Core dumps:** a dump holds task stacks, which may hold pool credentials, in on-device flash. `CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE` refuses a second dump until the partition is erased. Devices updated over the air keep their old 64K partition, because the partition table doesn't change that way.
- **Heartbeats before activation:** a lease whose heartbeats stop just before it activates can close sooner than 2,800 ms after activation, because activation counts as a heartbeat stamp and the link's poll counts from the earlier real one. It's an early, safe close.
- **Overheat detail:** the overheat path and the `unsafe_sample` facts of `worker_revocation_detail` are host-tested only.
- **Secrets:** the pool client's transient copies of grant credentials are not wiped. Grant fields over 255 bytes are refused, although the signed limit is 512.
- **Reasons:** after a local stop, the device's status reads `connectivity_lost`. The `qualification` block carries its reason, and an operator's pause reads `unsafe_observation` there.
- **Boot:** the Gate link starts after Wi-Fi, the NTP wait and ASIC initialization, and not at all if a cold ASIC initialization fails. A panic before that updates no receipt and doesn't count a boot.
- **Ledgers:** the budget ledgers have no cap but the largest safe integer, and `watchdog_alive` is read by the owner task itself.
