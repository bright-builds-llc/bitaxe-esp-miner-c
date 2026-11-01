# Revocation design

Step 6 of `docs/project/c-fork-handoff.md` starts here. The contract says a device revokes at 2,800 ms without a heartbeat and begins shutdown within 3 s, with dispatch and submission cut. This note settles where that happens in upstream's task structure, with the fewest upstream edits that can make it correct. Read against upstream `master` at `bb4fd0c`. `AGENTS.md` (Bright Builds Fork) and `standards/core/architecture.md` shaped it.

## Why the existing stop isn't enough

`mining_stop` is the only stop upstream has, and it can't carry this deadline:

- `power_management_task.c` runs it from a poll, and it may be inside a cooling wait or a `mining_start` when the deadline passes.
- `ASIC_initalized` is the flag other tasks check, and it stays true until the end of `mining_stop`. The frequency ramp before that takes 6 to 8 s on a 205, so jobs keep going to the chip and shares keep going to the pool throughout.
- `mining_paused` belongs to the operator. `POST /api/system/resume` clears it, and the overheat path restarts mining without looking at it.

## Paths to the chip and the pool

| Path | Single point | Sole production caller |
| --- | --- | --- |
| Work to the ASIC | `ASIC_send_job` (`components/asic/asic_submit.c`) | `create_jobs_task` |
| Shares to the pool | `stratum_submit_share` (`main/tasks/stratum_task.c`) | `ASIC_result_task` |
| Power up | `mining_start` (`power_management_task.c`), from the resume edge and the overheat restart | Power task only |
| Pool connection | `stratum_task` loop | `stratum_task` |

ASIC configuration writes (`ASIC_set_frequency`, `ASIC_set_nonce_space`, `ASIC_set_version_mask`, `ASIC_set_difficulty`) carry no work and the stop itself needs them, so they stay open. Results still drain from the chip after closure. They count as nonces, but none reaches the pool, even one that would meet the network target, because the device no longer has authority to submit.

## The design

A new component, `components/bwg_worker`, holds one atomic authority word that any task can read without a lock. Two admission checks read it at the two single points above, so closure takes effect on the next job and the next share, with no dependence on the power task's poll.

**Functional core: `bwg_authority`.** Four states and a pure transition function.

| State | Work and shares | Stop wanted | Leaves by |
| --- | --- | --- | --- |
| `LOCAL` | Admitted, which is upstream's behaviour | No | Revoke, to `STOPPING` |
| `LEASED` | Admitted | No | Revoke, to `STOPPING` |
| `STOPPING` | Refused | Yes | Stop confirmed, to `STOPPED` |
| `STOPPED` | Refused | Yes | Lease begins, to `LEASED`; or release, to `LOCAL` |

A device starts in `LOCAL`, the zero value, so a build with no Gate activity behaves as upstream does. Closure latches and the first reason wins. Authority can only reopen from `STOPPED`, so a new lease can't start until the chip has been reset, and results from an old lease can't be credited to a new one. That's why jobs need no generation stamp, unlike the Rust firmware's `WorkPermit`, and `asic_job_t` stays unchanged.

**Imperative shell: `bwg_revocation`.** The atomic word, plus two one-shot `esp_timer` deadlines: heartbeat at 2,800 ms and lease expiry. A callback closes authority directly from the `esp_timer` task, priority 22, above every mining task, so a busy or blocked worker task can't postpone it. A heartbeat restarts only its own timer, so it can't extend a lease.

**Upstream hooks.** Every one is a line or two.

| File | Hook |
| --- | --- |
| `components/asic/asic_submit.c` | Refuse the job before `ASIC_send_work` |
| `main/tasks/stratum_task.c` | Refuse the share in `stratum_submit_share`; stay off the pool while a stop is wanted |
| `main/tasks/power_management_task.c` | Add the stop to `wants_stop`, report stop start and finish, give up a start once revoked, revoke a lease on a local stop or overheat, and don't restart after cooling if revoked |
| `main/tasks/stratum_v1_client.c` | Leave the pool once a stop is wanted |
| `components/asic/frequency_transition_bmXX.c` | Abandon an upward ramp once a stop is wanted |

Anything that stops mining locally during a lease also revokes it: operator pause, hardware fault, no reachable pool, or overheat. A lease can't silently stop producing work while the device still reports it as active. A lease's overheat ends with the ordered stop, not upstream's safe mode. In `LOCAL` these calls do nothing, so upstream's automatic overheat restart still applies.

`mining_paused` stays the operator's flag, untouched, so `POST /api/system/resume` can't reopen anything.

The ordered stop drops the pool connection once the chip's voltage is off and the fan is at full, before the cooling proof. `stratum_request_reconnect` takes the transport mutex, which a stalled send can hold for its 5 s send timeout, and that must not delay the chip or the fan. The cooling proof has no upper limit, and a device that can't prove cooling has no business staying on a pool. Admission is already refusing shares throughout. The grant's pool is wiped when the stop finishes, or when the wait for it gives up at 140 s.

## How it's proved

A test variant, `CONFIG_BWG_REVOCATION_PROOF`, off by default and never set in a tracked configuration, adds a task that drives real leases with no serial protocol. Local work stands in for a grant's pool. It covers a handover from local mining, heartbeat loss and lease expiry under continuing heartbeats, and it logs one line per closure.

Pure logic and timers are tested under QEMU in `components/bwg_worker/test`, and `components/asic/test/test_asic_submit_revocation.c` covers the refusal in `ASIC_send_job`.

## Results

Ultra 205, 2026-10-31, 485 MHz and 1200 mV, Stratum V1 to the board's configured pool. Built from the commit that adds this section, before it was committed, on top of `a8b57a5` (reported as `v2.15.2rc0-61-ga8b57a5-dirty`). ESP-IDF v6.0.2. Proof image SHA-256 `c17fe5eee9a2c50201b51f1aa0d87f6ccceaac77eed1df8b8013eae97ae577f1`. Production image 2,908,048 bytes, SHA-256 `e5e6649499170723c4533c4cf1238822a2dabf2c30a3296bed243abf2bb338e0`.

| Closure | Heartbeat to close (ms) | Close to stop start (ms) | Stop length (ms) | Last job before close (ms) | Last share before close (ms) | Jobs refused | Shares refused | Shares sent after close |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Local handover | n/a | 35 | 16,651 | 512 | 211 | 3 | 0 | 0 |
| Heartbeat loss | 2,800 | 56 | 17,130 | 1,125 | 16,076 | 4 | 0 | 0 |
| Heartbeat loss | 2,800 | 2 | 17,112 | 1,115 | 3,431 | 4 | 1 | 0 |
| Heartbeat loss | 2,800 | 74 | 17,112 | 1,644 | 67,435 | 4 | 0 | 0 |
| Lease expiry | 0 | 49 | 17,121 | 1,810 | 129,636 | 4 | 0 | 0 |
| Lease expiry | 0 | 80 | 17,059 | 1,973 | 191,836 | 4 | 0 | 0 |

- **The 3 s limit:** on heartbeat loss, the stop began at most 2,874 ms after the last heartbeat, and work and shares were refused from 2,800 ms.
- **Expiry:** the lease closed on time with heartbeats still running, which shows they don't extend it. The 0 in that column is a heartbeat landing in the same millisecond.
- **Refusal is real:** in the second heartbeat loss, the chip returned a nonce of difficulty 12,031 against a pool difficulty of 1,000 during the stop. It was counted as refused and no `mining.submit` went out. Between 389 and 415 nonces arrived during each lease closure's stop, because the chip keeps hashing its last job through the frequency ramp.
- **Independent check:** "Shares sent after close" counts `mining.submit` lines in the serial log, separately from the admission counters. Log stamps ran 1,200 ms ahead of the timer in all six closures, measured from the closure lines themselves and removed first.
- **Recovery:** the board started mining again after every stop, and returned to local mining at the end. It finished with 15 accepted and 0 rejected shares, no faults or watchdog marks, about 434 GH/s and 61 C.
- **Production image:** 300 s in `LOCAL` gave 14 accepted and 0 rejected shares, about 417 GH/s and 60 C, with no proof code in the image. All 213 tests pass under QEMU, 23 of them new.

The last three closures have no recent share because this pool sometimes starts a new connection at difficulty 100,000 and lowers it about a minute later, and those 45 s leases ended first. The best nonces in them were 5,516, 3,276 and 3,224.

**Not exercised on hardware:** a stalled pool send, a closure during `mining_start` or the overheat cooling wait, a lease ended by pause, fault, lost pool or overheat, Stratum V2, a real Gate lease with its own pool, and the Gamma 601. The unit tests cover the authority logic of those stops, not the power task hooks.

## What this does not settle

- **One send already past the check:** closure can't recall a job or share that passed admission a moment earlier. The proof measures how late the last one was.
- **A closure during ASIC detection:** a start now gives up between its steps, and an upward ramp stops at the next step. What remains is the chip detection and setup before the ramp, about a second on a 205. `docs/project/gate-integration-evidence.md` has the measurement.
- **Operator state and API:** `miningPaused` stays false during a revocation stop, and the `pauseReason` and `stopping` fields don't exist.

The Gate's serial protocol, a grant's pool, the ordered stop, the reboot report and the 15 s quiet rule came after this note, and `docs/project/gate-integration-evidence.md` records them. The results above predate them and were measured on upstream's `mining_stop`.
