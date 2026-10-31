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

Not done. The board was not attached over USB on 2026-10-30, so it has no install or observation here.

## Effective settings for later steps

- `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`: logs share the USB channel, which conflicts with the Gate's one-writer rule.
- `CONFIG_BT_ENABLED=y`: BLE is on in this baseline. The owner's decision to turn it off is still to be made in the build.
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is not set, so the mark-valid call in `main/main.c` has no pending image to confirm. This supports the OTA rollback candidate in the handoff.
- Coredump to flash is off (`CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y`).
