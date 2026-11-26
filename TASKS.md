# Tasks

The fork's list of open work, one line per item: its state, what unblocks it, and where the detail lives. Detail stays in that doc rather than here. Delete an item when it's done, since git keeps the history.

## Waiting on the owner

- `task-upstream-sync`: `upstream/master` is 26 commits ahead (checked 2026-11-26), and merging it is a port, not a merge. #2062 to #2065 replace the pool model the Gate builds on: `stratum_request_reconnect()` and `SYSTEM_reset_pool_session` are gone, `work_received` and the share counters moved per pool into `pool.h`, and failover now runs primary and fallback pools as two concurrent tasks, so a lease's single connection to its own pool needs a new design. #2072 needs ESP-IDF 6.0.3. #2061 adds its own SV2 certificate-date check beside the fork's stricter one; keep one. The rest merged with small conflicts in `create_jobs_task.c`, `power_management_task.c`, `stratum_v1_client.c` and `test_sv2.c`. Needs the owner's go-ahead, then hardware requalification of the lease guarantees.
- `task-upstream-mark-valid`: open the mark-valid offer (an issue, linking branch `offer/mark-valid-after-startup`). See "Offer: mark valid after startup" in `docs/project/upstream-candidates.md`.

## Deferred

- `task-reference-source-release`: when a reference image first goes to anyone but the owner, build it with `tools/build_reference_image.sh` from a pushed commit, tag the commit, and publish its `SOURCE.txt`, `dependencies.lock` and `THIRD_PARTY_NOTICES.txt` beside the image. See `reference/README.md`.
- `task-upstream-font-licences`: Nippo and Angel Wish, inherited from upstream, may not be redistributable; accepted as a known risk on 2026-11-20. Revisit by replacing them (ideally upstream), asking the rights holders, or taking advice. See "Inherited fonts" in `reference/README.md`.
- `task-upstream-stalled-pool`: the pull request for leaving a stalled pool. See `docs/project/upstream-candidates.md`.
- `task-upstream-candidates`: the other rows with a next action, including the verified ASIC difficulty fix. See `docs/project/upstream-candidates.md`.
- `task-secured-ble-provisioning`: BLE setup with more security than upstream. BLE stays off in fork builds until this exists. The earlier notes are in the Rust repository's `TASKS.md`.
- `task-telemetry-descriptive-extensions`: descriptive telemetry names, if still wanted. The earlier notes are in the Rust repository's `TASKS.md`.

## Unexplained results

- `task-unanswered-pool-205`: the test pool's full backlog answered or reset new connections on the 205's run, where on the 601 they timed out, so connections that time out are unproven on the 205. See "The 205 on `f7139ad`" in `docs/project/gate-integration-evidence.md`.
- `task-start-no-work`: a Start on the 205 waited its 25 s for work from a real pool and timed out once, then passed 20 of 20. The log line now names its three conditions, so read that line if it happens again. See "A Start that reached no work" in `docs/project/gate-integration-evidence.md`.
- `task-long-cooling-205`: one 313 s lease on the 205 ran at 66 °C with its fan at 7,725 rpm and needed 36 s of cooling. Four others of that length held about 60 °C and needed about 12 s. The board shed less heat in that lease, and why isn't known. See "The long cooling" in `docs/project/gate-integration-evidence.md`.
- `task-second-reconnect`: 2 of the 601's 22 closed-connection runs connected a third time. The one examined followed about 9 s in which the board's whole network path went silent while its USB link carried on, so reconnecting was right. Why the path went silent isn't known. If it happens again, read the board's own log (`/api/system/logs`) soon after. See "The second reconnect" in `docs/project/gate-integration-evidence.md`.
- `task-power-loss-readings`: in its last reading before a power loss on the 601, the board read 81 °C and 16.2 W, up from 51 °C and 13.5 W a second before, as its input sagged. Whether the readings were real isn't known. See "Faults at the board" in `docs/project/gate-integration-evidence.md`.
- `task-white-screen-601`: a one-off so far. After a power-on on `1a6b464`, the 601's display stayed solid white while the board otherwise ran normally, and a software restart cleared it. It wasn't power-cycled again, so a cold start is only a suspect. Look again if it comes back.
- `task-discarded-reply-source`: on the 205, a reconnect about 1.5 s after an interrupted status read reported `helloRecovery.discardedReplies` 1, five times out of five, where reconnects through the chooser, about 4 s, reported 0. A reply in transit across the interrupt is a guess, and nothing has tested it. See "Ticket 28 on both boards" in `docs/project/gate-integration-evidence.md`.
- `task-loose-lcd-boot`: with its OLED loose on the shared I2C bus, the 205 ran its Gate link but never reached the network, though upstream falls back to a virtual display when the panel is absent and starts the web server even in degraded mode. Its console wasn't captured. Next: boot with the display unplugged to test the clean case. See "The lifecycle rerun on the 205" in `docs/project/gate-integration-evidence.md`.
- `task-chrome-crash-onboarding`: in Ticket 15's second install attempt on the 601, Chrome 157 stopped with `EXC_BREAKPOINT` on its IO thread, with no script frames, while the onboarding page reconnected to the C image in a loop. The fix removed the loop and no later sitting crashed, so the cause isn't known. If it happens again, keep Chrome's crash report. See "Ticket 15: onboarding on the 601" in `docs/project/gate-integration-evidence.md`.
