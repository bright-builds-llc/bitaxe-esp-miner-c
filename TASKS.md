# Tasks

The fork's list of open work, one line per item: its state, what unblocks it, and where the detail lives. Detail stays in that doc rather than here. Delete an item when it's done, since git keeps the history.

## Waiting on the owner

- `task-faults-at-the-board`: a pulled cable and a power loss need the owner at the board or a switchable supply. A real overheat needs the owner's say-so, as it risks damage.

## Deferred

- `task-upstream-stalled-pool`: the pull request for leaving a stalled pool. See `docs/project/upstream-candidates.md`.
- `task-upstream-candidates`: the other rows with a next action, including the verified ASIC difficulty fix. See `docs/project/upstream-candidates.md`.
- `task-bright-builds-update`: once `bright-builds-llc/bright-builds-rules` pull request 14 merges, run `bright-builds-auto-update.yml` once and confirm it passes.
- `task-secured-ble-provisioning`: BLE setup with more security than upstream. BLE stays off in fork builds until this exists. The earlier notes are in the Rust repository's `TASKS.md`.
- `task-telemetry-descriptive-extensions`: descriptive telemetry names, if still wanted. The earlier notes are in the Rust repository's `TASKS.md`.
- `task-secp256k1-lowmemory`: save about 1 MB with the `secp256k1` table setting. See `docs/project/c-fork-handoff.md`.

## Unexplained results

- `task-unanswered-pool-205`: the test pool's full backlog answered or reset new connections on the 205's run, where on the 601 they timed out, so connections that time out are unproven on the 205. See "The 205 on `f7139ad`" in `docs/project/gate-integration-evidence.md`.
- `task-start-no-work`: a Start on the 205 waited its 25 s for work from a real pool and timed out once, then passed 20 of 20. The log line now names its three conditions, so read that line if it happens again. See "A Start that reached no work" in `docs/project/gate-integration-evidence.md`.
- `task-long-cooling-205`: one 313 s lease on the 205 ran at 66 °C with its fan at 7,725 rpm and needed 36 s of cooling. Four others of that length held about 60 °C and needed about 12 s. The board shed less heat in that lease, and why isn't known. See "The long cooling" in `docs/project/gate-integration-evidence.md`.
- `task-second-reconnect`: 2 of the 601's 22 closed-connection runs connected a third time. The one examined followed about 9 s in which the board's whole network path went silent while its USB link carried on, so reconnecting was right. Why the path went silent isn't known. If it happens again, read the board's own log (`/api/system/logs`) soon after. See "The second reconnect" in `docs/project/gate-integration-evidence.md`.
- `task-white-screen-601`: a one-off so far. After a power-on on `1a6b464`, the 601's display stayed solid white while the board otherwise ran normally, and a software restart cleared it. It wasn't power-cycled again, so a cold start is only a suspect. Look again if it comes back.
