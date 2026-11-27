# Upstream merge at `3a531c1`

Branch `sync-upstream-20261126` merges `upstream/master` at `3a531c1` into the fork at `220d2dd`: 26 upstream commits, from #2072 (ESP-IDF 6.0.3) to #2128. This review names what each group of changes touches in the fork and which checks it invalidates, as the sync policy in `c-fork-handoff.md` asks. It reached `master` on 2026-11-27 on the owner's word, before any board ran it.

**Evidence:** CI on `5a95fabc` passed the firmware build on IDF 6.0.3, the QEMU unit tests (261, no failures), the BWG host tests (679, plain and sanitized) and the Bright Builds checks. **Non-claims:** nothing on this branch has run on a board, and none of the hardware evidence in `gate-integration-evidence.md` covers it.

## Pool model: #2062, #2063, #2064, #2065

Pool state moved into per-pool sessions (`main/pool.h`), protocols sit behind a driver registry, and primary and fallback pools run as concurrent tasks. `stratum_request_reconnect()`, `SYSTEM_reset_pool_session` and the module-wide work and share counters are gone.

The fork's port, with defaults taken without the owner:

- **Start and hand-back:** a lease's start and its return to local mining call `stratum_notify_pool_selection_changed`. It resets every pool's session, clears the pool failures and wakes the primary task, which could otherwise sit in its 30 s retry wait past the 25 s Start deadline.
- **Stops:** a stop, and a revocation while cooling, call `stratum_abort_connection`.
- **Operator resume:** after the restart guard, an operator's resume also calls `stratum_notify_pool_selection_changed`.
- **Readiness and shares:** a lease reads its first work and its share counts from the primary pool's session. The restart guard reads local hashing from the active session.
- **Fallback:** no fallback pool while a lease is active. The fallback task exits, forced fallback is skipped, and a primary that gives up during a lease reports the pools unavailable, which ends the lease as before.

Invalidated: a lease reaching work within 25 s, the stop within its limit, a lease ending on lost pools, the lease's share counts, the local restart guard, and the operator's pause and resume around a lease.

## Stratum V2: #1876, #1878, #2045, #2050, #2061, #2085, #2128

The V2 client now keeps one connection per pool, opens it asynchronously, and handles more messages.

- **Hooks:** the fork's V2 hooks follow the per-pool connection, and the connect observation wraps the new asynchronous connect. #2085 records a share's timestamp before the send, which now sits between the fork's submission begin and end.
- **Certificate dates:** #2061 adds a certificate-date check beside the fork's stricter one. Both stay, the fork's first, and upstream's can't refuse what the fork's passes. Keeping only one is the owner's call.
- **Key material:** #2128 stops logging Noise handshake key material, which a lease's log quieting already hid during a lease.

Invalidated: the V2 attempt records (frames, timings, nonces by job slot), the authority-key handshake, and the certificate-date refusal.

## ASIC and jobs: #2009, #2036, #2041, #2054, #2093, #2097

- **Difficulty:** #2036 removed the difficulty controller, and `asic_initialize` now writes the difficulty after every init. The fork's `asic_init.c` fix is therefore dropped, and its upstream-candidates row says so.
- **Jobs and shares:** #2009 pauses job creation on disconnect and drops stale shares. #2093 adds job rolling and timeouts, and #2097 moves the miner job into `components/mining_job`. The fork's revocation hook in `create_jobs_task.c` clears the scheduler's work, and `asic_submit.c` and the job-slot hooks are unchanged.

Invalidated: revocation refusing work and shares, the ASIC difficulty after a stop and restart, and the nonce-to-dispatch link in the V2 records.

## Platform: #2072, #2086, #2101, and others

- **IDF 6.0.3 (#2072):** the newer IDF grew the PSA SHA-256 state, so the V2 record now holds libsodium's own state type.
- **Networking (#2086, #2101):** LwIP core locking, DSCP marking and larger TCP and Wi-Fi buffers change timing on the network path.
- **The rest:** #2029, #2030, #1737, #2046, #2080 and #2099 touch no fork hook.

Invalidated: the telemetry cadence qualification, the crash-loop guard and the startup mark-valid check on a fresh image, since the platform under them changed.

## Not reviewed here

The web UI changes that came in through the merge weren't checked against the Gate's fields beyond CI's Angular build and unit tests. Upstream's own tests pass, but not on a board.
