# Upstream candidates

Step 5 of `docs/project/c-fork-handoff.md`. Checked 2026-10-30 against upstream `master` at `bb4fd0c`, by reading source unless a row says otherwise. Upstream pull requests were open as of that date.

| Candidate | State on `bb4fd0c` | Next action |
| --- | --- | --- |
| Failed sensor reads | Still there: `EMC2101.c` returns -1, which never trips the 75 C check | Covered by open #2058. Carry nothing |
| Zero fan speed | Still there: `fan_rpm` triggers no stop | Covered by open #2060, with #1995 for the zero-count division. Carry nothing |
| Single reading trips overheat | Still there. Seen on a 601, see `baseline.md` | Covered by open #2059 and issue #2000. Our log is evidence for them |
| Manual fan after recovery | Still there. Measured on a 601: 100% manual fan survived a reboot at 52 C | No overlapping pull request found. Fix verified on a 601 with one forced trip per image: without it the fan ended manual at 100% at 42 C, with it auto fan returned, at 26% and 51.6 C. Open as #2073 from branch `fix/overheat-keep-auto-fan` |
| Cooling gate | Still there. `mining_stop` clears `ASIC_initalized`, so `Thermal_get_chip_temp` returns -1 before it reads the sensor, and the 45 C check in the cooling loop never holds on the 8 boards marked `emc_internal_temp`. The comment there says these readings stay valid | Fix second. Proving it needs an Ultra 205 attached |
| `www` upload offset | Still there: writes at `size - remaining`, so a short image lands at the end of the partition | Small pull request, no overlap found |
| `www` erase failure | Still there: `ESP_ERROR_CHECK` panics during erase | Small pull request, no overlap found |
| OTA rollback | Changed: `main.c` now calls the mark-valid function, but no rollback setting is on, so no image is ever pending | Open an issue and ask, because field bootloaders may not gain it |
| Stop order and results | Changed in part: frequency now winds down to 50 MHz first. It still cuts voltage before holding reset and before marking the ASIC uninitialized, and it ignores results | Open an issue and ask, with no evidence of harm yet |
| Overheat step-down | Mostly gone: #1790 clamps voltage to the regulator's minimum. The literal fallbacks remain, but need settings at or below 100 to apply | No action |

## Security-sensitive topics

Bounds on settings accepted over HTTP, secrets in logs and over BAP, and authentication on BLE writes were checked privately on this tree. They go through upstream's private reporting flow in `SECURITY.md`, so this public file names topics only.
