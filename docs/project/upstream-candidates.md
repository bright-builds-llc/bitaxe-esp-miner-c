# Upstream candidates

Step 5 of `docs/project/c-fork-handoff.md`. Checked 2026-10-30 against upstream `master` at `bb4fd0c`, by reading source unless a row says otherwise. Upstream pull requests were open as of that date.

| Candidate | State on `bb4fd0c` | Next action |
| --- | --- | --- |
| Failed sensor reads | Still there: `EMC2101.c` returns -1, which never trips the 75 C check | Covered by open #2058. Carry nothing |
| Zero fan speed | Still there: `fan_rpm` triggers no stop | Covered by open #2060, with #1995 for the zero-count division. Carry nothing |
| Single reading trips overheat | Still there. Seen on a 601, see `baseline.md` | Covered by open #2059 and issue #2000. Our log is evidence for them |
| Manual fan after recovery | Still there. Measured on a 601: 100% manual fan survived a reboot at 52 C | No overlapping pull request found. Fix verified on a 601 with one forced trip per image: without it the fan ended manual at 100% at 42 C, with it auto fan returned, at 26% and 51.6 C. Open as #2073 from branch `fix/overheat-keep-auto-fan` |
| Cooling gate | Still there. `mining_stop` clears `ASIC_initalized`, so `Thermal_get_chip_temp` returns -1 before it reads the sensor, and the 45 C check in the cooling loop never holds on the 8 boards marked `emc_internal_temp`. The comment there says these readings stay valid | No overlapping pull request found. Fix verified on a 205 with one forced trip per image, all cooling from 60 C: without it every cycle read -1 and the board restarted after 6 cycles, with it the sensor read 45 down to 39 C, and with the limit lowered to 38 C for a test image the count restarted on each reading above the limit until 6 cycles held, 76 s against 46 s. The hold at the real 45 C limit was not exercised, since the first reading was already 45 C. Open as #2074 from branch `fix/cooling-gate-reads-sensor` |
| `www` upload offset | Still there: writes at `size - remaining`, so a short image lands at the end of the partition | Small pull request, no overlap found |
| `www` erase failure | Still there: `ESP_ERROR_CHECK` panics during erase | Small pull request, no overlap found |
| ASIC difficulty after a restart | Still there. A stop resets the chip, but `current_difficulty_power` keeps its old value, so `difficulty_controller_update` sees no change and never writes the mask again. Measured on a 205 at 485 MHz with one pause and resume per image: unmodified `9a687c2` went from 100 nonces in 300 s, lowest difficulty 256, to 28,789 nonces in 286 s, lowest difficulty 1. With the fix it went from 92 nonces in 300 s to 104 in 305 s, lowest difficulty 258.7, and shares kept being accepted | Fix verified and carried on `master`. Branch `fix/asic-difficulty-after-restart` is ready to open as a small pull request. No overlap found in a title and body search |
| OTA rollback | Changed: `main.c` now calls the mark-valid function, but no rollback setting is on, so no image is ever pending | Open an issue and ask, because field bootloaders may not gain it |
| Stop order and results | Changed in part: frequency now winds down to 50 MHz first. It still cuts voltage before holding reset and before marking the ASIC uninitialized, and it ignores results | Open an issue and ask, with no evidence of harm yet |
| Overheat step-down | Mostly gone: #1790 clamps voltage to the regulator's minimum. The literal fallbacks remain, but need settings at or below 100 to apply | No action |

## Security-sensitive topics

Bounds on settings accepted over HTTP, secrets in logs and over BAP, authentication on BLE writes, and Stratum V2 certificate handling and handshake logging were checked privately on this tree. They go through upstream's private reporting flow in `SECURITY.md`, so this public file names topics only.
