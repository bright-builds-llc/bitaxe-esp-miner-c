# Tasks

The fork's list of open work, one line per item: its state, what unblocks it, and where the detail lives. Detail stays in that doc rather than here. Delete an item when it's done, since git keeps the history.

## Waiting on the owner

- `task-gate-bm1370-chip-ids`: the Gate's V2 parser refuses `coreId` above 111 and `smallCoreId` above 7, and most of the 601's V2 share attempts stop with `v2_evidence_invalid`. The fix belongs in the Gate repository. See "Gamma 601" in `docs/project/gate-integration-evidence.md`.
- `task-core-dump-exposure`: decide whether dumps may stay in on-device flash, since their task stacks may hold pool credentials. See "Non-claims" in `docs/project/gate-integration-evidence.md`.
- `task-reference-firmware`: decide whether to call an image reference firmware under the owner's signed capability. Images hold a test authority until then. See "Non-claims" in `docs/project/gate-integration-evidence.md`.
- `task-start-readiness`: rule on a Start waiting for first work, up to 25 s, where Rust's source reports running once a pool authorizes. See "Differences from Rust" in `docs/project/gate-integration-evidence.md`.
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
- `task-second-reconnect`: one of the 601's closed-connection runs reconnected again 28 s after the fault, and three later runs didn't. See "Gamma 601" in `docs/project/gate-integration-evidence.md`.
- `task-dropped-trace-event`: `trace` read `droppedEvents` 1 on `1a6b464`, where the image before it read 0, and the event wasn't identified. See "Gamma 601" in `docs/project/gate-integration-evidence.md`.
