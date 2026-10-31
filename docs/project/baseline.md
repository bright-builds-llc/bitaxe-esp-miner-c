# Baseline of the unmodified fork

Step 4 of `docs/project/c-fork-handoff.md`. Recorded 2026-10-30. No firmware source differs from upstream at this commit.

## Build

| Field | Value |
| --- | --- |
| Source commit | `9a687c2cf3ca4eaa73d2d83db52b69e46884963c` (upstream `bb4fd0c` plus documentation only) |
| Reported version | `v2.15.2rc0-55-g9a687c2` |
| ESP-IDF | v6.0.2, `esp32s3` |
| `esp-miner.bin` | 2,906,528 bytes, SHA-256 `b925312473b9d60b79d7fd61fd5279e1905dd54525df2db82f7ffad9761870e3` |
| Free in the 4 MiB slot | 1,287,776 bytes |

The same `esp-miner.bin` serves every board, and NVS config selects the board. The build's `npm i` rewrote `main/http_server/axe-os/package-lock.json`, so the embedded web UI came from that rewritten lockfile. The file was restored afterward and is not committed. The factory image holds this binary, the bootloader, the partition table, initial OTA data and an NVS image generated from `configs/config-<board>.csv`.

## Config differences from the stock CSV

The NVS image sets the owner's Wi-Fi, sets the owner's pool as both primary and fallback (stock points at upstream's defaults), and sets `selftest` to 0. Frequency and voltage stay at stock values.

## Ultra 205

Installed from a Rust production image by factory install, after the base-MAC digest matched and the full flash was backed up privately.

| Behaviour | Measured |
| --- | --- |
| Write | 15,802,368 bytes, hash verified |
| Boot | Two clean boots, about 16 minutes and then 10 minutes, with no panic, watchdog or brown-out marker |
| Frequency, core voltage | 485 MHz, 1200 mV set |
| Hashrate | 430 GH/s 10-minute average at the end of the second boot, 433.6 GH/s expected |
| Power, input | 12.2 W mean, 5.27 V mean |
| Temperature, fan | 60 C reading under auto fan, about 4,300 RPM at 41% |
| Shares | 34 accepted, 0 rejected in 600 s, no fallback, no overheat |
| Internal heap free | About 92,100 bytes, steady over the 600 s |

The console log showed one error line, `wifi: sta is connecting, cannot set config`, early in each boot, and Wi-Fi connected anyway. Opening the USB port reset the chip even with DTR and RTS held low, so the second boot began when monitoring began.

**Non-claims:** no self-test, overheat, power-loss, OTA or BLE exercise; no run longer than the two boots above; no Gate behaviour. The 205's `bwg_worker` identity is no longer on the device, and exists only in the private backup.

## Gamma 601

Installed from a Rust production image by factory install, with the same binary, after the base-MAC digest matched and the full flash was backed up privately. Write: 15,802,368 bytes, hash verified.

**At stock settings upstream's overheat stop tripped, possibly on one reading.** At 525 MHz and 1150 mV, it tripped 689 s after boot on an ASIC reading of 76.875 C.

| Uptime | ASIC | Regulator |
| --- | --- | --- |
| 72 s | 66.5 C | 56 C |
| 253 s | 72.5 C | 70 C |
| 524 s | 72.9 C | 74 C |
| 614 s | 73.5 C | 76 C |
| 690 s | 76.9 C | 76 C |

Before the trip it averaged 1,060 GH/s (10-minute figure) at 20.5 W with the fan near 100% and 7,900 RPM, and had 51 shares accepted and 0 rejected. The fan controller's filtered temperature held between 73.4 and 73.8 C for the 87 s before the trip, and logged 73.7 C about 1.8 s before it. The regulator reading did not change. So the trip may have come from a brief spike or a bad read and not from sustained heating, as upstream issue #2000 describes, but the log cannot separate those. The margin is thin either way: 73.5 C sustained at full fan is 1.5 C under the limit. Room temperature was not measured, and the handoff records one earlier trip on Rust at the same settings.

**Recovery.** Upstream cut power and saved 425 MHz, 1050 mV, manual fan and 100% fan speed to NVS. The observer also requested a pause at that moment, so the board stayed stopped after cooling, at 21 mV core, about 5 W and 30% fan. A watched reboot then ran at the saved settings:

| Behaviour | Measured, 600 s |
| --- | --- |
| Hashrate | 865 GH/s 10-minute average, 867 GH/s expected |
| Power, input | 13.6 W mean, 5.25 V mean |
| Temperature, fan | ASIC 51.8 C and regulator 53 C at most, 100% fan at about 8,150 RPM |
| Shares | 39 accepted, 0 rejected, no fallback, no overheat |
| Internal heap free | 92,203 bytes, steady |

The board keeps 425 MHz and 1050 mV until someone changes them. After later fan testing, the baseline app was reinstalled with auto fan turned back on. Over 240 s that held about 59 C at 35% fan.

**Seen on this device, relevant to the handoff's upstream candidates:**

- Manual fan after recovery: 100% fan survived the reboot, even with the chip at 52 C.
- `overheat_mode` in `/api/system/info` read 0 throughout the trip, because `main/system.c` loads it once at boot.
- During the cooling wait, the API kept returning the last power and ASIC temperature readings from before the stop.
- With power cut, the ASIC temperature reads -1.

**Non-claims:** no self-test, power-loss, OTA or BLE exercise; no long run; no stable run at stock settings; no Gate behaviour. The 601's `bwg_worker` identity is no longer on the device, and exists only in the private backup.

## Effective settings for later steps

- `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`: logs share the USB channel, which conflicts with the Gate's one-writer rule.
- `CONFIG_BT_ENABLED=y`: BLE is on in this baseline. The owner's decision to turn it off is still to be made in the build.
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is not set, so the mark-valid call in `main/main.c` has no pending image to confirm. This supports the OTA rollback candidate in the handoff.
- Coredump to flash is off (`CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y`).
