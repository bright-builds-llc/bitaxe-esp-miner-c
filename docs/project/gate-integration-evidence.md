# Gate integration evidence

Evidence for Bitcoin Work Gate parity with the Rust firmware, recorded 2026-11-01. Every claim below is about this one image on this one board, unless a line says otherwise, and none carries over from a Rust image or an earlier image of this tree. The Gamma 601 has its own section and image, and nothing else here covers it. "The 205 on `f7139ad`" is the latest run on this board, recorded 2026-11-02, and the sections before it are about earlier images.

| | |
|---|---|
| Board | Bitaxe 205 (BM1366), 485 MHz, 1200 mV, matched by base-MAC digest before each ROM write |
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
| `attempt_diagnostic` | 63, 73 | 63 was refused, see "A refused Start". 73 passed: 3,085 ms active, 2 nonces. `worker-mining-progress-v2` reports filter `bm1366-ticket-256-leading-zero-40-v1`, 4 matches, 0 misses |
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

## A refused Start, fixed after this image

Attempt 63 was refused with `command_rejected` about 200 ms after the Gate sent Start, on an idle board at 33 °C. Its record reads `unsafe_observation`, 0 ms active, closed 1 ms after activation and with no stop stage run, and the ledger charged its 30,000 ms. It happened once in this session's 73 attempts, and attempt 73, on the same image, passed. The image at the top of this file carries the defect.

On a paused board the power task read the pause and then ended any lease, and a local stop reports as `unsafe_observation`. A lease that opened between that read and that call ended at once. It erred on the safe side: no work ran and the chip stayed off, but the Gate saw a failed Start and a spent allowance. The task now reads whether a lease is open before it reads the flags. A lease clears its pause before it opens, so an early read of the pause can't end it.

A temporary 400 ms delay after the flag read, never committed, widened the gap on two test images. Without the fix, 3 of 4 leases from a paused board were refused, each logging `local_stop` with no stop stage run. With the fix and the same delay, 6 of 6 started and stopped normally, with no `local_stop` logged.

The clean image with the fix is `esp-miner.bin` SHA-256 `2b3f0c9bb12cb41ba3cf577b0fdf0ea7c764d84ff0ffab0295a7eca163d03ee9`, ELF `5c5166e7587235257d3e041b6a616b55929025f25d855d8713e98618080d3fb5`, an uncommitted tree on `f6f06bb` that reports that commit. This is all that ran on it:

- **Checks:** 248 of 248 in QEMU. The default build is 2,977,344 bytes with no proof marker, the proof build 2,978,320 bytes with its marker once, and the test image 2,982,496 bytes.
- **Frontend:** `npm run test:ci`, 123 of 123. This wasn't run before `f6f06bb` was pushed, and CI failed there on a test mock missing `mineOnBoot`, which this tree fixes. The firmware unit tests passed in CI.
- **Settings:** the same five tags as below.
- **Harness:** all 11 scenarios ended as in the table below, with the same serial failures. A pause during a lease still ended it, logged as `local_stop`.
- **Acceptance:** `attempt_normal` 74, `attempt_diagnostic` 75, 76 and 77, and `attempt_heartbeat` 78 all passed, and the ledger read 8,640,000 ms. Heartbeat closes read 2,800 and 2,801 ms.
- **Logs:** 16 device logs hold no `task_wdt`, "Task watchdog", `usb_tx_failure`, panic or abort line.
- **Close probe:** 0 failures in 600 rounds, in 143 s.
- **Restoration pages:** `completion`, `pause`, `cancel`, `expiry`, `monotonic_uncertainty`, `disconnect` and `reboot` passed in a full run. `authorization_negatives` failed there, as the next section says. Run with `reboot` before it, it then passed 3 of 3, each round rejecting the replayed renewal and Start.
- **Acceptance through the page:** the trace dropped 0. Restart, receipts and the core dump self-test completed. The soak ran 28 renewals and 600,000 ms active with 34 shares accepted, and a later `attempt_normal` ran 9 renewals with 14 accepted. Cadence v1 passed its idle, USB and mining phases, with no send failures.
- **Core dump:** 56,160 bytes decoded, with the receipt the image above reports.
- **V2 and Noise:** through the harness, plain Noise and the channel were accepted, a wrong authority was rejected, a cancel was cancelled, and a share was acknowledged. Through Chrome, the three page attempts first failed to open the port (`serial_operation_failed`) straight after the harness runs. The cause wasn't established. Run after the restoration pages, Noise, the channel and a share were all accepted.
- **Guards and fan stop:** the crash-loop guard paused after 3 abnormal resets, took a lease and cleared its streak. The restart guard held at 627 s after 34 connections, and a lease from the hold worked. A real fan stop ended a lease with `zero_fan`.
- **Later logs:** 11 more device logs hold none of those lines. The two heartbeat closes they record read 2,801 ms.

- **Silent chip and failed starts:** a temporary line, never committed, silenced the chip on a test image of this code, `esp-miner.bin` SHA-256 beginning `94ba32e4736f6c83`. Leases from a paused board were refused with `command_rejected` at 28.9 s, twice. After a resume, failures came 34, 64, 124 and 244 s apart in the device log, and the fifth held with `restart_guard` at 473 s. A lease from that hold was also refused at 28.9 s, and the board returned to the hold with power off.

A race this rare can't be shown absent by passes on the clean image, so the delayed test is the evidence for the fix. It covers the pause. A hardware fault or pool failure flag set just as a lease opens would still end that lease, and that wasn't exercised.

## A Start with no work

`authorization_negatives` failed once on the clean image with `serial_failure: timeout`, 30,008 ms after the Gate sent Start, with the board otherwise healthy. No device log was captured for it, so its cause isn't established.

The same outcome can be made on demand. On that image, on the 205, a lease whose pool accepted the connection and never answered ended in `serial_failure: timeout` at 30.2 s. The board logged "Lease did not reach work within 25000 ms" and began its stop, and 35 s after the Gate gave up it was paused with core voltage off. A failed Start runs the stop before it replies, and a live chip's stop takes about 20 s, so the refusal can't arrive inside the Gate's 30,000 ms limit. The silent chip fits because it has no live chip to stop, at 28.9 s.

This is left as it is. From reading their source, not running them, Rust also stops before it replies, and its wait on a pool that never authorizes runs to the lease's end, so it should reach the same timeout. The Gate treats a timeout and a refused Start alike, closing with `control_failed` and sending no restore. No run here compared the two firmwares, and a pool that merely answers slowly wasn't tried.

A pool that authorized and never sent a job ended the same way on this image, with `serial_failure: timeout` at 30.3 s and the board paused with core voltage off. Rust's source differs here, as "Differences from Rust" says.

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

A chip whose replies were cut over JTAG then gave real failed starts: retries came 30, 60, 120 and 241 s apart, and the fifth failure held with `restart_guard` at 494 s. After a reset and a resume, hashing returned in about 10 s. Those retries came from this image, but a lease on the silent chip did not: the test's debugger script ended partway in every try on it, so those runs say nothing either way. A test image of the fixed tree, with one temporary line that cuts the chip's replies and was never committed, gave that lease twice from a paused board. Both were refused with `command_rejected` at 28.9 s, inside the 30 s limit, after 0 chips were detected and the 25 s wait for work ran out, and the board stayed paused with its core voltage off. A lease on a chip silenced from a held `restart_guard` state read the same on the earlier image `981a6404`, at 28.86 s, with the pause and its reason returned.

## The 205 on `f7139ad`

Everything in this section is about this image on this board, recorded 2026-11-02. It's the first run of the stalled-pool change and the lost-pool reason on a BM1366.

| | |
|---|---|
| Board | Bitaxe 205 (BM1366), 485 MHz, 1200 mV, matched by base-MAC digest before its ROM write |
| Source | `f7139ad`, built from a clean worktree and an empty build directory. Its firmware source equals `1a6b464`. Only docs changed in the main tree during the run |
| Image | `esp-miner.bin` SHA-256 `0f00cabc8b2754458f6aa32f7ebec8719e17c96d4beb41decc3e9a98a97414f0`, reporting `v2.15.2rc0-65-gf7139ad` |
| ELF | SHA-256 `c4a921dc2ff2ad4d35fd19a9fb2149a38803a0f5f9bc90f2ef9c814854373366` |
| Gate, trust, pool | As above: the same Gate commit, a test authority and a real pool, with the Rust repository's fixture for V2 and Noise |

| Check | Result |
|---|---|
| Close probe | 600 rounds, 0 failures, 145 s |
| The Gate's eight pages, in Chrome | All eight closed with no first failure, each with its expected device reason, and `authorization_negatives` passed in the full run. Start replies 11.0 to 11.2 s |
| Harness | `connect`, `start_abort` (`heartbeat_timeout`), `limit_fault` (`unsafe_observation`, 6,297 ms active), `reviews` (`monotonic_reset`), `replay` (`command_rejected`, signature `valid`), `lease_stop` (stop 23,068 ms), `expiry` (`lease_or_budget_expired`, 19,932 ms active), `heartbeat_loss` (`heartbeat_timeout`) and `reboot` ended as expected. `operator_pause` failed its first run, see below. The `mining` lease ran 313,191 ms with 17 accepted, 0 rejected, 138 nonces and 15 renewals. Its stop took 52,620 ms, 35,999 ms of it cooling |
| Campaign | Not on this image. Its three windows were spent on earlier images, so the attempts below stand in |
| `attempt_normal` | Ordinals 83 and 88 passed: 178,106 and 178,847 ms active, 12 and 13 accepted, 0 rejected, 78 and 83 nonces |
| `attempt_diagnostic` | Ordinal 84 passed: 2,721 ms active, 1 nonce |
| `attempt_foreground`, `attempt_heartbeat` | Ordinals 85 and 86 passed. `heartbeat_timeout`, closed at 2,800 ms both times, stop started at 2,895 and 2,827 ms |
| `soak` | Passed. Exactly 600,000 ms, 28 renewals, 40 accepted, 0 rejected, 237 nonces, 635 samples, 0 unsafe, judged on the chip's real final reading |
| `cadence` | Ordinal 87. All three phases passed, longest interval 555,916 µs, longest execution 56,450 µs, 0 send failures, 0 dropped |
| `trace` | Available, `droppedEvents` 0 |
| `restart`, `endpoint` | Completed, identity matched |
| Core dump | 56,128 bytes, crashed task `bwg owner`, 69 threads. Receipt `{0x50434f32, 0x402, 0x402, 0x400, 0x703, 0x53544631}` |
| V2 and Noise | Noise accepted through the harness and the page, a wrong authority rejected, cancel cancelled, the channel accepted through the harness and the page. The page's share in that step connected on its seventh try, 46 s in, and the cause of the wait wasn't read |
| V2 shares | 3 of 3 accepted: 1 through the harness and 2 through the page, each on its first attempt. None was refused with `v2_evidence_invalid`, which fits the BM1366's chip IDs being inside the Gate's limits |
| Crash-loop guard | Tripped after 3 abnormal resets, and a lease from that state started and stopped. With and without a lease first, the resume mined and logged "Streak cleared", and one more abnormal reset left the board mining with no pause reason |
| Restart guard | Held at 625 s after 34 connections, then 1 more connection. A lease from the hold started and stopped, and the board recovered |
| Fan stop | Duty 0 ended the lease as `zero_fan`, reason `unsafe_observation`, 1,014 ms after the last safe sample, 11,302 ms active. Settings were restored |
| Starts that can't succeed | A pool that never answers and a pool that sends no job each ended in `serial_failure: timeout`, at 30.2 s, with the board paused and its core off |
| Device logs | Those written during this run hold no `task_wdt`, "Task watchdog", `usb_tx_failure`, panic or abort line |

**Pool faults during a lease.** These ran over V1 or V2 against the same local pools as the 601's, at difficulty 1, with each V1 fault about 40 s after the first share. The Gate's harness lost its session as each lease ended, so these read the device's own record. Each row ended with the board paused and its core off.

| Fault | Result |
|---|---|
| Connection closed once, pool still listening | Reconnected within 0.1 s, and the lease ran on: 15 renewals, 1,638 more shares, 0 rejected, stop 28,721 ms. There was no second reconnect, in the one run |
| Connection closed and connections refused | The lease ended itself about 6 s later, reason `control_failed`. Stop 19,224 ms |
| Pool stops reading and sending, twice | The failed send left the connection. After 3 more connections the lease ended itself about 99 and 112 s after the fault, reason `control_failed`. Stops 27,813 and 28,674 ms. The 601 took about 67 and 71 s, and this board's shares come about half as often |
| New connections meant to go unanswered, 4 runs | The fault wasn't produced. The host's own probe was answered 3 times and reset once, where on the 601 it timed out, so this says nothing about connections that time out. Each lease ended itself about 6 s after the fault with no connection reaching the pool, reason `control_failed`, with stops about 19.5 s. The test host's OS changed between the 601's run and this one, and whether that's why wasn't established |
| V2 fixture killed | The lease ended itself as the fixture died, reason `control_failed`. Stop 19,176 ms |
| V2 fixture frozen | The board didn't notice the stall. Sends kept succeeding, and the attempt ended about 148 s later, reason `control_failed`, closing as it sent its last share. That fits the 16-frame limit the 601 pointed to, at half the rate, but the frame count wasn't read here. Stop 27,121 ms |

**A Start that reached no work.** On the first `operator_pause`, the board was idle and paused, the Gate connected, and its first Start ended in `serial_failure: timeout` at 30.2 s. The board's log reads "Lease did not reach work within 25000 ms", with the chip up after about 11 s and 14 s more passing with no work. This used the real pool, right after `reboot`. That wait needs an initialized chip, first work and a fresh safety sample, the log doesn't say which was missing, and pool logs are silent during a lease, so its cause isn't known. Run again with `reboot` before it, `operator_pause` passed 5 of 5, each time ending a lease with the pause, then taking a lease from the paused board. It may be the same as the `authorization_negatives` timeout under "A Start with no work", which had no log.

The log line now names all three. On a test image of this source plus that change (`esp-miner.bin` SHA-256 starts `2f9137085f39dff6`), a pool that never answers and a pool that sends no job each logged "chip ready 1, work received 0", with a sample age of 22 and 33 ms. Those are made failures, so they show the line works and say nothing about the real pool's case. No image with the line has met that case, and the run above is from the image without it.

The same test image then took 15 more Starts from the real pool, each a `lease_stop` right after `reboot`. All 15 reached work, none logged the line, and their stops took 19,517 to 19,868 ms. With the 5 above, that's 20 passes since the one timeout, so repeating further is a poor way to catch it. The line stays in the image for the next time it happens.

**The long cooling.** The `mining` lease that needed 35,999 ms of cooling was already hot before its stop. Its fan held 7,725 rpm and its chip settled at 66 °C on about 13.4 W. Three repeats of the same length on the test image held 60 to 61 °C at about 4,600 to 5,200 rpm on about 12.4 W. They cooled in 10,999 to 12,004 ms and stopped in 27,371 to 28,468 ms, with 15 or 16 accepted and 0 rejected. Each stop had its fan near full speed, 7,769 to 8,095 rpm by its end, so the slow proof came from the board shedding less heat in that lease, not from the fan or the stop. Why it shed less wasn't established, since the board reads neither ambient temperature nor airflow and nobody was watching it. It had passed by the `soak` about 30 minutes later, which peaked at 61 °C.

Non-claims for this run:

- **Runs:** the checks above ran once on this image, in one sitting, except where a row says otherwise.
- **Stop length:** stops here took 19 to 53 s, longer than the 601's fixed proof in the worst case and 23 s longer than the earlier `mining` stop on this board. The long ones spent up to 36 s cooling to the proof's limit, and "The long cooling" above says what's known about the longest.
- **Not repeated:** the silent chip and failed-start pacing need a temporary test image, and their rows above are from earlier images. Those code paths didn't change.
- **Pool faults:** the 601's non-claims on test pools apply here too. A pool that stops reading was exercised on V1 only for its failed send, as on the 601.
- **Comparison:** the 601's `trace` dropped 1 event and this board's dropped 0, but with one run each that doesn't say why.

## Gamma 601

Everything in this section is about this image on this board. None of it carries over to the 205, or from the 205's sections.

| | |
|---|---|
| Board | Bitaxe Gamma 601 (BM1370, no enable pin, no board temperature sensor), 425 MHz, 1050 mV, matched by base-MAC digest before its ROM write |
| Source | `1a6b464`, clean. The tools ran as committed there, and only this file changed after it |
| Image | `esp-miner.bin` SHA-256 `b6d39d96d9e925de7eba285536165773ba7674c07b3c4ffbf94d7236df611e68`, built from an empty build directory, reporting `v2.15.2rc0-64-g1a6b464` |
| ELF | SHA-256 `a2b53ddc73fff478e22df16f9453edc65e03a7d628adca4c661104d6df4a794d` |
| Gate, trust, pool | As above: the same Gate commit, a test authority and a real pool, with the Rust repository's fixture for V2 and Noise |

| Check | Result |
|---|---|
| Close probe | 600 rounds, 0 failures, 140 s |
| The Gate's eight pages, in Chrome | All eight closed with no first failure, each with the same device reason as the 205. Start replies 10.3 to 10.4 s |
| Harness | `connect`, `start_abort`, `limit_fault`, `replay`, `lease_stop`, `heartbeat_loss`, `reboot`, `operator_pause`, `reviews` and `expiry` completed. The `mining` lease ran 312,468 ms with 23 accepted, 0 rejected, 110 nonces and 15 renewals. Stops took 48,071 and 47,366 ms |
| Campaign | Not on this image. A device spends its three windows once, and this one's identity is kept, so the attempts below stand in. On `af80095` all three passed: `normal` 178,944 ms active, 16 accepted, 0 rejected, 74 nonces; `foreground` `link_closed`, closed at 2,739 ms; `heartbeat` `heartbeat_timeout`, closed at 2,801 ms |
| `attempt_normal` | Ordinals 17 and 22 passed: 178,407 and 178,404 ms active, 12 and 13 accepted, 0 rejected, 60 and 73 nonces |
| `attempt_diagnostic` | Ordinal 18 passed: 1,931 ms active, 1 nonce |
| `attempt_foreground`, `attempt_heartbeat` | Ordinals 19 and 20 passed. `heartbeat_timeout`, closed at 2,800 ms both times, stop started at 2,857 and 2,808 ms |
| `soak` | Passed. Exactly 600,000 ms, 28 renewals, 27 accepted, 0 rejected, 231 nonces, 654 samples, 0 unsafe |
| `cadence` | Ordinal 21. All three phases passed, longest interval 543,887 µs, longest execution 44,487 µs, 0 send failures, 0 dropped. Its window's own check passed too, and judges only the device's stop, since the page stops renewing on purpose |
| `trace` | Available, with `droppedEvents` 1 where `af80095` read 0. The dropped event wasn't identified on this image, and "The dropped trace event" below names the kind |
| `restart`, `endpoint` | Completed, identity matched |
| Core dump | Panic reset seen, identity matched. 56,128 bytes, crashed task `bwg owner`, 69 threads. Receipt `{0x50434f32, 0x2, 0x2, 0x0, 0x703, 0x53544631}`: the pins cut are reset alone, as this board has no enable pin, where the 205's receipt also names enable |
| V2 and Noise | Noise accepted through the harness and the page, a wrong authority rejected as `authority_verified/authentication/wrong_authority`, cancel cancelled, the channel accepted through the harness and the page. Shares are below |
| Crash-loop guard | Tripped after 3 abnormal resets with `pauseReason` `crash_loop`, and a lease from that state started and stopped. The campaign's log read showed no "Streak cleared" line after the resume, so it was tested directly, reading the board after each reset. With and without a lease first, the resume mined and logged the clear, and one more abnormal reset left the board mining with no pause reason |
| Restart guard | Held at 623 s after 34 connections, then 1 new connection in 150 s. A lease from the hold started and stopped, and the board recovered |
| Fan stop | Duty 0 ended the lease as `zero_fan`, reason `unsafe_observation`, 1,043 ms after the last safe sample. The stop drove the fan to 100%, and settings were restored |

A stop on this board takes about 48 s by design, within the Gate's bound: about 16 s of tail, then a cooling proof that always waits 30 s, because the chip has no reading once its power is off. The proof also needs the regulator at 95 °C or below and a turning fan, three readings in a row. Three things follow, and each was seen here:

- **Status during a stop:** the Gate gives up on a status after 30 s, and a status in flight during this stop has no reply until it ends, so the session is lost. From its source, Rust's controller also blocks during a stop, for up to 140 s, but Rust wasn't run this way. `gate_host.ts` and `page_run.ts` wait out the stop on this board before asking.
- **Hello during a handover:** a Hello to a board mining locally has no reply until the handover's stop ends, about 50 s. With the board still mining locally, `completion` and `pause` timed out connecting. Both passed once each run began from a paused boot, connecting on the first attempt.
- **Final reading:** the board reports no chip temperature at the end of a stop, so `soak` was refused as `terminal_cooling_missing` and Rust's window judge has no rule for it. See the judge non-claim below.

**V2 shares and the Gate's parser.** Of twelve share attempts on this image, one was accepted, through the page. The other eleven, eight through the harness and three through the page, stopped with `v2_evidence_invalid` once work was dispatched. On `af80095`, two of five were accepted, one through each. The chip IDs below were read on that image. Both tools parse a status before keeping it, so no refused record exists to read. The Gate's `web/worker-v2-serial.ts:42` refuses a `coreId` above 111 or a `smallCoreId` above 7, which fits the 205's BM1366 (112 cores, 3 small-core bits). The BM1370 has 128 cores and 4 bits. Across the 601's logs, 249 nonces reached core 127 and small core 15, with 23 above the core limit and 140 above the small-core limit. Across the 205's, none of 3,335 did. The refused harness attempt logged a share at core 48, small core 12, and the two refused page attempts have no such log, so their IDs weren't read. Given an accepted record with only those IDs changed, the parser accepts 111/7 and refuses 112/7, 111/8 and 48/12. The firmware reports the chip's real IDs, and from its source Rust's decoder reports the same ranges, so this isn't a difference from Rust. The limit is in the Gate, and until it changes a share on this board passes only when its IDs happen to fit.

**After the Gate's fix.** The Gate's `eb6a770` accepts a `coreId` up to 127 and a `smallCoreId` up to 15. With the Gate checkout at that commit and no local changes, the 601 ran ten more share attempts, six through the harness and four through the page, on the `f7139ad` image (`esp-miner.bin` SHA-256 starts `0f00cabc8b275445`), each on a fresh paused boot. All ten reached `accepted` with no failure, and no private copy of the parser was used. The shares' IDs ranged up to core 127 and small core 14, and eight of the ten had a core of 112 or more or a small core of 8 or more. The fork's judge, `tools/bwg_gate/v2_judge.mjs`, runs the Rust repository's protocol judge, and that judge refused exactly those eight with `v2_asic_fact_domain`. Its `scripts/str005-v2-serial/protocol-shares.mjs:37` and `execution-receipts.mjs:47` hold the same BM1366 limit, `coreId < 112 && smallCoreId < 8`. The two inside it were accepted, 1 share through the harness and 2 through the page. With that judge widened to the same range as the Gate's, in one helper both checks call, in the Rust repository's `f2e1e2ff`, all ten were judged `acceptedProtocol`: 7 shares through the harness and 5 through the page. Its 455 tests pass, including new ones that pass 111/7 through 127/15 and refuse 128/15 and 127/16. Non-claims: the records were judged again, not taken again. The rest of this section ran against `b7751451`, not `eb6a770`.

**Starts that can't succeed.** These ended safely, each with the board paused, its core off and a Gate timeout. None of them got a refusal, because a failed Start stops before it replies and this board's stop outlasts the Gate's 30 s limit. The Gate treats a timeout and a refused Start alike. "Test" is the image described below.

| Check | Image | Result |
|---|---|---|
| Pool that accepts and never answers | This | `serial_failure: timeout` at 30.2 s, after "Lease did not reach work within 25000 ms" |
| Pool that authorizes and sends no job | This | `serial_failure: timeout` at 30.2 s, with the same log line |
| Silent chip, twice | Test | `serial_failure: timeout` at 30.3 s each time, where the 205 was refused at 28.9 s. Chip detection failed within about 1 s, and the Start still waited its full 25 s |
| Failed-start pacing | Test | Retries came 35, 64, 124 and 243 s apart, and the fifth failure held with `restart_guard` at 473 s. The 205's were 34, 64, 124 and 244 s, also holding at 473 s |
| Lease from that hold | Test | `serial_failure: timeout` at 30.3 s. The board stopped and stayed paused with `restart_guard` |

The test image is `af80095`'s source plus one temporary line that cuts the chip's replies, built and removed in one step and never committed. Its `esp-miner.bin` SHA-256 starts `7145409d060d658a`. It reports `af80095`'s version string, so only its hash tells them apart. These three rows weren't repeated on this image, which doesn't touch the chip's path.

**Pool faults during a lease.** These ran over V1 against a local test pool, at difficulty 1, with each fault 40 s after the first share. `af80095` is the image before the change under "Fixed by this evidence" (`esp-miner.bin` SHA-256 `32156fdc7e3dc6c21ed51eb9da31e42577675ccb574e3dc5abc0d7dccb5f5d6d`), and its rows show what that change replaced. An image of the same change built before its commit gave matching results on the faults it ran, which were all of those below but the closed connection with the pool still listening.

| Fault | Image | Result |
|---|---|---|
| Connection closed once, pool still listening | `af80095` | Reconnected within 0.1 s, and the lease ran on: 15 renewals, 3,179 more shares, 0 rejected, stop 47,322 ms |
| Connection closed once, pool still listening, 4 runs | This | Reconnected within 0.1 s each time, and each lease ran on: 15 renewals, 3,154 to 3,262 more shares, 0 rejected, stops 47,270 to 48,226 ms. One of the four reconnected a second time 28 s after the fault, and the other three didn't. Pool logs are silent during a lease, so its cause wasn't found here. "The second reconnect" below finds a short loss of the board's network path |
| Pool stops reading and sending | `af80095` | The lease ran to its end with nothing accepted after the fault. 52 sends each blocked for 5 s, the result task handled 2,267 nonces where a run with no stall handled 3,723, and 11 chip frames were corrupted. Stop 47,623 ms |
| Connection closed and connections refused | This | The lease ended itself within about 7 s, reason `control_failed`, and the board paused. Stop 47,260 ms |
| Pool stops reading and sending, twice | This | The failed send left the connection. After 3 more connections the lease ended itself at about 67 and 71 s, reason `control_failed`, and the board paused. Stops 47,456 and 48,001 ms |
| Connection closed and new connections unanswered | This | A host probe timed out, and the pool saw no further connection. The lease ended itself within about 22 s, reason `control_failed`, and the board paused. Stop 48,442 ms |
| V2 fixture killed | This | The lease ended itself as the fixture died, reason `control_failed`. Stop 47,313 ms |
| V2 fixture frozen | This | The board didn't notice the stall. Sends kept succeeding, and the attempt ended about 75 s later, reason `control_failed`, closing 1 ms after its last share, which fits the record's limit of 16 share frames. Stop 47,975 ms |

The Gate's harness lost its session during each of those stops, as a status in flight does on this board. The V2 runs used a private copy of the harness that keeps heartbeats going, since that scenario's own fault is suppressing them, and a private copy of the Gate's parser with the chip limits widened to the BM1370's. A first attempt at the last three faults was void and is left out of the table: the test pool's backlog still answered connections, and the harness died on the parser's limit.

**The same stall off a lease, on upstream's source.** Upstream's `bb4fd0c` (`esp-miner.bin` SHA-256 starts `e85b1ae4127641d8`) and the same source with the lease condition removed from the fix (starts `65c7e39604dca7fa`) ran with no Gate against the same kind of pool, with a healthy second test pool as the fallback, and were watched for 270 s after each fault. Unpatched, a pool that stopped reading and sending kept the board for the whole watch: 50 failed sends, each holding the result task for 5 s, 1,707 nonces, 10 corrupted chip frames, and no contact with the fallback. Patched, the board reached the fallback in 69 and 72 s with 2,971 and 3,041 nonces and no corrupted frames, and went back when the pool recovered. With no fallback it reached upstream's "all pools unreachable" pause in about 65 s and resumed 16 s after the pool recovered. A pool that stopped reading one connection while still sending jobs kept the unpatched board too, with 51 failed sends and 1,713 nonces, while the patched board reconnected in 16 s and handled 3,194. Every failed send in the unpatched runs reported 0 bytes written, so none of them showed a partial line.

**The dropped trace event.** Two test images logged each drop with its values: source `2384894` plus logging (`esp-miner.bin` SHA-256 starts `b5d3cf8a73ce8fb6`), and source `8df16c9` with the same logging, pool warnings on and no line per nonce (starts `1172e0d864ba9a32`). `trace` read `droppedEvents` 0, 0 and 1 across three boots, and the board's own log, which survives a restart, held the drop behind the 1. Four drops were logged in all, one per boot, over 18 leases and those three boots. All four were stage `dispatch_started`, each with its epoch equal to the current one, 99 to 1,111 wire bytes and 0 queued. Those values pass every other check in `bwg_trace_note`, which leaves only the busy flag, held by another task's note at that moment. The note gives up rather than wait, and counts the drop. Rust's recorder does the same (`serial/trace.rs`, `try_lock`), and the Gate's export only checks that the count is a number, so this is the design working rather than a fault. Non-claims: which task held the flag wasn't logged, and Rust's rate of these wasn't measured.

**The second reconnect.** On the first test image, one of six closed-connection runs connected a third time. It came 77.6 s after the fault, not 28 s as on `1a6b464`, so it isn't tied to the fault. For about 9.4 s in that run, the board's WebSocket log, its HTTP API (an 8 s poll timed out) and its pool connection all went silent together. They came back within 3 s of each other, and the pool saw a reset on the old connection and then a new one. Meanwhile the Gate's USB link kept every 20 s renewal on time, and the 20 s window that held the gap gained 104 accepted shares where its neighbours gained 227 and 258. The Mac's own Wi-Fi log shows its link steady through that window. The second image ran 12 more runs and none of them reconnected a second time. Its log held one beacon timeout, during local mining after a lease, with the board logging normally again 6 s later. So the second reconnect follows a short loss of the board's network path while the board kept running, and reconnecting was the right response. Non-claims: why the path was lost, whether that was the board's radio, the access point or the air, wasn't found. The board's own log of that window was overwritten before it was read. The case on `1a6b464` wasn't examined and is only assumed to be the same. How often Rust or upstream sees this wasn't measured.

Non-claims for this board:

- **Profile:** the board ran the 425 MHz, 1050 mV, manual 100% fan settings already in its NVS, not its stock profile, so nothing here speaks to stock settings or the catalog's top end.
- **Identity:** NVS held no `bwg_worker` before this board's first Gate image, so its identity is new, and carrying Rust's identity over wasn't exercised on this board.
- **Overheat flag:** `overheat_mode` was already 1 from an earlier trip and was cleared by hand before flashing. That trip wasn't investigated here.
- **Judge:** `acceptance_supervisor.ts` shows Rust's window judge the stop's cooling proof as a 45 °C final reading, on this board only, when the stage is `fan_paused` and the reading is null. The judge's other checks, including the stage and the fan, still run, and records on disk are unchanged. This is a substitution for the judge's check, not a temperature measurement. The `soak` check allows the same substitution.
- **Runs:** the checks above ran once on this image, in one sitting, except where a row says otherwise. Eleven of the twelve V2 share attempts were refused by the Gate's parser, so the harness has no accepted share on this image, only the page. "After the Gate's fix" has the later run on `f7139ad`.
- **The 205:** the same firmware source ran on the 205 under "The 205 on `f7139ad`", as its own image and with its own results.
- **Pool faults:** a local test pool and a small receive buffer stand in for a real pool's failure. The connections that time out were dropped by a full backlog on the test host, not by a lost network path. A V2 attempt's record wasn't read after the fixture died, since the session ended with it, so the cause of that stop is inferred from its timing and its reason. A stalled V2 pool never reached a failed send, and an attempt can't: its record's limit of 16 share frames fits in the board's 5,760-byte send buffer however small the pool's buffer is. The fix's failed-send path is unexercised over V2. A pool that answers a new connection and then stalls with no share to send would wait until the lease ends.
- **Silent chip:** its replies were cut by that line, on a healthy chip. A chip that has really failed may behave differently, and Rust wasn't run against a silent chip or either pool here.

## Fixed by this evidence

- **Heartbeat stamp:** a heartbeat close could report 2,799 ms, which Rust's share judge would refuse as `v2_fault_device_deadline`. The stamp was taken after the timer restarted, and fixing that wasn't enough: 1 in about 10 still read 2,799 on the rerun, because a close through the link's poll counts from when the link saw the heartbeat. The reported stamp is now that same moment, and every close on this image reads 2,800 or 2,801 ms.
- **Frame read timing:** a V2 share attempt was refused with `v2_operation_deadline`, because the timer started before the idle wait, so a pool quiet for 14.8 s looked like a slow read. It now starts at a frame's first byte, as Rust's does. An observed frame now gives up after 10 s, and an observed Act Two read does too, where before only the end of authority stopped either.
- **Send and receive operations:** upstream sends from one task while another waits to receive, and they shared one open-operation slot, so a send closed a read in progress. Each direction now has its own. `test_sv2.c` holds the regression tests for both.
- **Close:** a Close that arrived while another record was still draining lost the host's final credit, so the host's Close timed out. Revoking an already closed epoch wiped the closing receipt, where Rust revokes only a current epoch. A record fully queued but still draining also counted as a partial line when something interrupted its wait, which blocked the final credit, and fixing that took the probe from 11 of 600 failures to 0. `test_writer_close.c` holds the regression tests.
- **Cooling proof:** the judge refused `attempt_normal` with `terminal_cooling_proof_missing`, because the proof passed on one reading of 45 °C and the sensor's whole degrees flickered back to 46 °C on the next. The proof now wants 3 readings in a row.
- **Heartbeat reason:** the judge refused `attempt_heartbeat` with `heartbeat_timeout_not_proven`, because the link's poll could see the lost heartbeat before the revocation timer and call it `link_closed`. Both now name it `heartbeat_timeout`, as Rust's deadline check does.
- **Telemetry priority:** the telemetry loop now runs where the Gate's cadence contract names, which turned all three phases from `passed: false` to `passed: true`.
- **Stalled pool:** a pool that stopped reading never broke the connection, so each share held the result task for the 5 s send timeout, and the lease hashed on with nothing accepted. A failed send can also leave part of a line on the wire. A lease now leaves the connection after a failed send, as Rust's source does, and runs out of attempts if the pool doesn't recover. Off a lease upstream's behaviour is unchanged.
- **Reason for lost pools:** a lease that ran out of pools reported `unsafe_observation`, which reads as a safety fault. It now reports `control_failed`, as Rust's source does.

An earlier version of this file said the owner forbade core dumps, that Close matched Rust, and that the telemetry priority should stay at 2. All three were wrong: the first had no source beyond that file, the second hid a bug in this tree, and the third gave up a check the Gate makes.

## Reference firmware under the owner's capability

On 2026-11-03 the owner chose to make a reference image. A Gate capability signs no image: the owner's Update Authority signs a board, a firmware name and version, and the SHA-256 of the Worker Serial manifest, and the Gate pins the image itself by its source commit and ELF digest. The owner's capabilities for the 205 and the 601, already signed for the Rust image and public in the Rust repository's `firmware/bitaxe/bwg/`, name `bright-builds-reference-firmware` 0.1.0, the only name the Gate's trust contract allows, and bind manifest digest `a43c2c7d61f95fc6`. This tree's `BWG_SERIAL_MANIFEST_JSON` hashes to the same digest, and both signatures verify against the owner's public trust. So the C image needs no new signature: built with `BWG_TRUST_DIR` holding that trust and those two capabilities, it is reference firmware.

On the Gamma 601, that build of `57d7be9` (`esp-miner.bin` SHA-256 starts `8f0472df47605b9c`, ELF `46c4a0b1cded501f`) held the owner's key IDs and the 601's signed capability and none of the test authority's. The Gate's admission at `eb6a770`, given only the owner's public trust and that commit and ELF, admitted the board on its first attempt and read its baseline. Given the test authority's trust instead, it refused the board at the `capability` stage. The 601 was then returned to the `f7139ad` test image.

The owner then ran the harness's `lease_stop` on the same image with their own Work Lease key, through a script that checked the key was one the image trusts and put the test image back afterwards, so no agent used the key. Admitted on the first attempt, the Start replied in 10,267 ms, the renewal 20 s later was accepted, and in 47 s of mining the board submitted 7 shares, with 7 accepted and none rejected, and every one of 27 nonces matched the expected filter. The operator stop replied in 47,568 ms with the safe stop complete at `fan_paused`, the reason `restoration_requested` and the restoration confirmed as `paused`, the 601's usual stop of tail and cooling proof. An earlier try ended at Ctrl-C 43 s into its stop, after the same Start, renewal and mining. Non-claims: one lease, through the harness and not the owner's Gate page. The 205 wasn't run. The capabilities name no commit and were signed before this tree existed, so a later manifest change needs new signatures.

## Differences from Rust

By decision:

- **Pause during a lease:** ends the lease here. Rust keeps mining until the lease ends. A pause is the operator's direct request, so it stops the work.
- **Resume during a lease from a paused board:** the pause still returns afterwards, because the resume can't be told apart from the lease's own running state.
- **A Start's readiness:** here the board waits for its first work, up to 25 s, and a pool that authorizes and sends no job fails the Start. From its source, Rust reports running once a V1 pool authorizes or a V2 channel opens, without waiting for work, but Rust wasn't run against such a pool. The owner kept this on 2026-11-03, since first work proves the pool and chip path.

Not in this image, and required by no Gate path:

- **Cadence schema:** this image reports `worker-telemetry-cadence-v1`, and Rust reports v2. The Gate accepts both. v2 adds only `maximumLiveStagesUs` and `worstInterval`, whose 11 stages are those of Rust's telemetry loop. This loop has one stage, so they would read 0.
- **Startup diagnostics:** `allocation_failure_context`, `statistics_startup`, `storage_http_*`, `wifi_startup_failure`, `usb_memory_checkpoint`, `bwg_worker_start_failure` and non-ready `usb_startup` stages aren't emitted. Rust emits some of these on healthy boots too. Only Rust's install judges read them, and those bind Rust's source. Adding them would touch `main.c`, `filesystem.c`, `http_server.c` and Wi-Fi.
- **Mining progress counters:** `stale_completion`, `duplicate_candidates`, the core and address-interval discards, and the blocked `wrong_session`, `job_lookup`, `work_stale` and `target_mismatch` counts always read 0. The expected filter is a software expectation, not a reading of the chip's mask.
- **Regulator temperature:** a `vr_temperature` fact in `worker_revocation_detail` is dropped by the Gate's grammar, which doesn't list it. Rust's has the same label and the same drop.
- **Panic receipt:** `rust_panic_receipt` describes a Rust panic, so it has no meaning here.

By construction, each small and stated so nobody relies on it:

- **Pool retries in a lease:** here 3 attempts come 2 s apart, and a connection that had work clears the count. From its source, Rust waits 5 s between 3 attempts, about 15 s when refused, and its count carries across connections. Rust wasn't run this way. Rust also blocks dispatch as soon as the pool is lost, where this chip keeps hashing the old job until the stop, with its shares refused.
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

- **Hardware not run:** a pulled cable, a power loss and a real overheat were not exercised on either board. USB re-enumeration leaves the chip powered, so it stands in for a pulled cable from the board's side only.
- **Trust:** this image holds a test authority. "Reference firmware under the owner's capability" has the build that holds the owner's.
- **Judges:** Rust's full operator and install judges bind Rust's source, and nothing C-aware replaces them yet, so `noise_judge.mjs` and `v2_judge.mjs` use only its pure protocol judges and prove less. Rust's share judge predates ADR-0037's amendment, so `v2_judge.mjs` shows it the reported headroom minus the old 15,550 ms reserve.
- **Restart guard:** its count lives in RAM, so a reboot starts over. Its hashing clock can include time spent cooling after an overheat. After a backoff ends, the next run can wait up to one more probe interval.
- **Duration:** the longest lease here is the 600 s soak. Nothing supports a claim about unbounded mining.
- **Core dumps:** a dump holds task stacks, which may hold pool credentials, in on-device flash. `CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE` refuses a second dump until the partition is erased. Devices updated over the air keep their old 64K partition, because the partition table doesn't change that way. The owner kept dumps in flash on 2026-11-03.
- **Heartbeats before activation:** a lease whose heartbeats stop just before it activates can close sooner than 2,800 ms after activation, because activation counts as a heartbeat stamp and the link's poll counts from the earlier real one. It's an early, safe close.
- **Overheat detail:** the overheat path and the `unsafe_sample` facts of `worker_revocation_detail` are host-tested only.
- **Secrets:** the pool client's transient copies of grant credentials are not wiped. Grant fields over 255 bytes are refused, although the signed limit is 512.
- **Reasons:** after a local stop, the device's status reads `connectivity_lost`. The `qualification` block carries its reason, and an operator's pause reads `unsafe_observation` there.
- **Boot:** the Gate link starts after Wi-Fi, the NTP wait and ASIC initialization, and not at all if a cold ASIC initialization fails. A panic before that updates no receipt and doesn't count a boot.
- **Ledgers:** the budget ledgers have no cap but the largest safe integer, and `watchdog_alive` is read by the owner task itself.
