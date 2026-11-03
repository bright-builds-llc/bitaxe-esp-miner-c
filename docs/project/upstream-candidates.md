# Upstream candidates

Step 5 of `docs/project/c-fork-handoff.md`. Checked 2026-10-30 against upstream `master` at `bb4fd0c`, by reading source unless a row says otherwise. Upstream pull requests were open as of that date.

| Candidate | State on `bb4fd0c` | Next action |
| --- | --- | --- |
| Failed sensor reads | Still there: `EMC2101.c` returns -1, which never trips the 75 C check | Covered by open #2058. Carry nothing |
| Zero fan speed | Still there: `fan_rpm` triggers no stop | Covered by open #2060, with #1995 for the zero-count division. Carry nothing |
| Single reading trips overheat | Still there. Seen on a 601, see `baseline.md` | Covered by open #2059 and issue #2000. Our log is evidence for them |
| Manual fan after recovery | Still there. Measured on a 601: 100% manual fan survived a reboot at 52 C | No overlapping pull request found. Fix verified on a 601 with one forced trip per image: without it the fan ended manual at 100% at 42 C, with it auto fan returned, at 26% and 51.6 C. Opened as #2073, and a maintainer closed it on 2026-11-03: the switch to manual is intended, so the user learns the board overheated. Not carried. Branch `fix/overheat-keep-auto-fan` kept. A comment recording that intent is open as #2076 from branch `docs/overheat-manual-fan-intent` |
| Cooling gate | Still there. `mining_stop` clears `ASIC_initalized`, so `Thermal_get_chip_temp` returns -1 before it reads the sensor, and the 45 C check in the cooling loop never holds on the 8 boards marked `emc_internal_temp`. The comment there says these readings stay valid | No overlapping pull request found. Fix verified on a 205 with one forced trip per image, all cooling from 60 C: without it every cycle read -1 and the board restarted after 6 cycles, with it the sensor read 45 down to 39 C, and with the limit lowered to 38 C for a test image the count restarted on each reading above the limit until 6 cycles held, 76 s against 46 s. The hold at the real 45 C limit was not exercised, since the first reading was already 45 C. Open as #2074 from branch `fix/cooling-gate-reads-sensor` |
| `www` upload offset | Still there: writes at `size - remaining`, so a short image lands at the end of the partition | Small pull request, no overlap found |
| `www` erase failure | Still there: `ESP_ERROR_CHECK` panics during erase | Small pull request, no overlap found |
| ASIC difficulty after a restart | Still there. A stop resets the chip, but `current_difficulty_power` keeps its old value, so `difficulty_controller_update` sees no change and never writes the mask again. Measured on a 205 at 485 MHz with one pause and resume per image: unmodified `9a687c2` went from 100 nonces in 300 s, lowest difficulty 256, to 28,789 nonces in 286 s, lowest difficulty 1. With the fix it went from 92 nonces in 300 s to 104 in 305 s, lowest difficulty 258.7, and shares kept being accepted | Fix verified and carried on `master`. Branch `fix/asic-difficulty-after-restart` is ready to open as a small pull request. No overlap found in a title and body search |
| Stalled pool | Still there. A pool that stops reading never breaks the connection, so each share holds the result task for the 5 s send timeout, and nothing reconnects or fails over. Measured on a 601, see below | Deferred by the owner on 2026-11-02, with nothing filed. The fork carries the fix during a lease only. See [the section below](#deferred-stalled-pool-pull-request) |
| OTA rollback | Changed: `main.c` now calls the mark-valid function, but no rollback setting is on, so no image is ever pending | Open an issue and ask, because field bootloaders may not gain it |
| Stop order and results | Changed in part: frequency now winds down to 50 MHz first. It still cuts voltage before holding reset and before marking the ASIC uninitialized, and it ignores results | Open an issue and ask, with no evidence of harm yet |
| Overheat step-down | Mostly gone: #1790 clamps voltage to the regulator's minimum. The literal fallbacks remain, but need settings at or below 100 to apply | No action |

## Deferred: stalled pool pull request

Plan if this is picked up: one public pull request that references issue #1964, with no separate issue. Only the pool the owner configured can cause this, it costs that miner only its work on that pool, and upstream tracks the sibling case in public. Open #1965 doesn't cover it: its 3-minute read deadline never fires while a pool that has stopped reading keeps sending jobs.

**The change:** on a branch from `upstream/master`, take the `stratum_task.c` change from `1a6b464` without its `bwg_mining_lease_pool_active()` condition, so `stratum_submit_share` always shuts the socket down after a failed send. A copy sits uncommitted on the local branch `fix/leave-stalled-pool`.

**Draft title:** Leave the pool connection after a failed share send

**Draft body:**

> A pool that stops reading never breaks the connection, so when its TCP window closes each share holds the ASIC result task for the 5 s send timeout, and nothing ever reconnects or fails over, because the receive loop only leaves on a read error. `stratum_submit_share` now shuts the socket down after a failed send, which fails the next read and lets the existing retry and fallback logic run. This complements #1965, whose read deadline doesn't fire while the pool keeps sending jobs. Tested on a Gamma 601 (BM1370) at `bb4fd0c` against a local test pool at difficulty 1 with a 1 KB receive buffer, for 270 s after each fault. When the pool stopped reading and sending, unpatched firmware stayed on it with 50 sends blocked, handled 1,707 nonces and never contacted the fallback, while patched firmware reached the fallback in about 70 s (twice) and handled 2,971 nonces. When the pool stopped reading one connection but kept sending jobs, unpatched firmware stayed stuck with 51 sends blocked and 1,713 nonces, while patched firmware reconnected in 16 s after one failed send, with 3,194 nonces and 3,049 shares accepted. A stalled pool that returned was picked up again, by the fallback's existing probe with a fallback and by the existing resume without one. With no fallback a stalled pool now reaches the existing "all pools unreachable" pause, in about 65 s, where before the board kept hashing with nowhere to send the shares. QEMU tests pass (190, 0 failures). SV2 and TLS share this path but weren't run, and a pool's normal share rate reaches the first failed send far more slowly than this test does.

**Evidence:** Gamma 601 (BM1370). Unpatched is upstream `bb4fd0c`, image SHA-256 starting `e85b1ae4127641d8`. Patched is the same plus this change, starting `65c7e39604dca7fa`. Each case ran once unless marked, and the fallback was a healthy second test pool. Raw logs weren't kept.

| Fault | Image | Outcome | Nonces handled | Sends that blocked 5 s | Chip frames with a bad preamble |
| --- | --- | --- | --- | --- | --- |
| Stops reading and sending | Unpatched | Stayed on it, fallback never contacted | 1,707 | 50 | 10 |
| Stops reading and sending | Patched | On the fallback in 69 s and 72 s (two runs) | 2,971 | 4 | 0 |
| Stops reading, keeps sending jobs | Unpatched | Stayed on it, accepted frozen at 602 | 1,713 | 51 | 10 |
| Stops reading, keeps sending jobs | Patched | New connection after 16 s, 3,049 accepted | 3,194 | 1 | Not counted |
| Stops, no fallback | Patched | Existing pause after about 65 s, retries every 35 s | 561 | 4 | 0 |
| Returns after 150 s, with fallback | Patched | Probe switched back 195 s after the fault | 3,041 | 4 | Not counted |
| Returns after 150 s, no fallback | Patched | Resumed 16 s after it returned | 636 | 4 | Not counted |

**Non-claims:**

- **Share rate:** the test pool drew about 10 shares a second, so buffers filled within about 15 s. By estimate, not measurement, a real pool takes over an hour to reach the first failed send, and #1965's deadline would fire first on a pool that also went silent.
- **Partial lines:** all 101 failed sends on the unpatched runs reported 0 of 301 bytes sent, so the code comment's "can leave part of a line" comes from reading `stratum_socket_send_all`. Cut that sentence to claim only what was seen.
- **Coverage:** one board, a test pool, V1 without TLS. No real pool was stalled.

**Before filing:**

- Open #2064 rewrote the same lines of `stratum_submit_share` when this was drafted, so check whether it landed and rebase. Rerun the table's cases if the code moved.
- Expect questions on the no-fallback case: a stalled pool now pauses mining after about 65 s. That's upstream's existing response to unreachable pools, but it's a visible change.
- After resuming from that pause, shares arrived about 20 times more slowly at a normal hash rate. That fits the "ASIC difficulty after a restart" row, though this run didn't prove it.
- If it merges, drop the fork's lease condition and the reason given for it under `stratum_task.c` in `AGENTS.md`.

## Security-sensitive topics

Bounds on settings accepted over HTTP, secrets in logs and over BAP, authentication on BLE writes, and Stratum V2 certificate handling and handshake logging were checked privately on this tree. They go through upstream's private reporting flow in `SECURITY.md`, so this public file names topics only.
