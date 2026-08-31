# ksn2-firmware

ZMK firmware configuration for the KSN-2 split keyboard — a 70% split derived from the original [KSN-1](https://github.com/Kesaros44/ksn1-firmware) design, reusing several of its proven patterns (custom LED drivers, `wakeup-source` handling for the peripheral half).

* Keyboard Maintainer: [AJG](https://github.com/Kesaros44)
* Hardware Supported: KSN-2 split keyboard, nice!nano v2 (nRF52840), BLE

## Hardware

- **MCU:** nice!nano v2 (nRF52840) per half, wireless (BLE)
- **Central:** the **left** half (unlike KSN-1, where central is the right half)
- **Matrix:** 6 rows x 9 columns per half, 6 rows x 18 columns combined; `diode-direction = "col2row"` — rows are inputs, columns are outputs
- **Encoder:** one EC11 encoder, left half (central) only
- **LEDs (left/central only):** a Caps Lock LED and a BLE connection-status LED
- **Backlight:** PWM backlight (pin `P0.06` on both halves), off at boot by default, auto-off when idle
- **RGB underglow:** not present on this board
- **Battery:** reporting enabled on both halves; central also fetches the peripheral's battery level and reports it to the host

### Pinout

Rows are identical on both halves:

| Row | GPIO |
| --- | --- |
| row1 | P1.00 |
| row2 | P0.24 |
| row3 | P0.22 |
| row4 | P0.20 |
| row5 | P0.17 |
| row6 | P0.08 |

Columns are identical on both halves:

| Column | GPIO |
| --- | --- |
| col1 | P0.11 |
| col2 | P1.04 |
| col3 | P1.06 |
| col4 | P1.01 |
| col5 | P1.02 |
| col6 | P1.07 |
| col7 | P1.11 |
| col8 | P0.10 |
| col9 | P0.09 |

Left-only extras:

| Function | GPIO |
| --- | --- |
| Encoder A / EN1 | P1.15 |
| Encoder B / EN2 | P1.13 |
| Caps Lock LED | P0.02 |
| BLE status LED / indicator-led | P0.29 |
| Backlight PWM | P0.06 |

### LED drivers

- **Caps Lock LED** — driven by ZMK's built-in `zmk,indicator-leds` node + `HID_INDICATOR_CAPS_LOCK`. This works here because it's on the **central** (left) half; that ZMK API is central-only and would fail to link on the peripheral (this is exactly the limitation KSN-1's custom `ksn1_peripheral_indicators.c` had to work around, since its Caps Lock LED is on its peripheral half).
- **BLE status LED** (`indicator-led`/`ble_led`) — driven by a custom driver, `config/src/ksn2_conn_status.c`: solid when the host is connected, blinking every 500ms when not, re-checked live on every `zmk_ble_active_profile_changed` event plus a 1s connected-state poll as a safety net. This replaced the `zmk-poor-mans-led-indicator` module's `CONFIG_INDICATOR_LED_SHOW_BLE` widget (still imported in `west.yml` but disabled via `CONFIG_INDICATOR_LED_WIDGET=n`), because that widget only re-evaluates on a BT-profile-switch event — once that one event passed, the LED just stayed in whatever state the blink sequence ended on, regardless of whether the host was still actually connected. (Ported from the same fix already proven on the Nexplit Keypad's `keypad_indicator.c`.)

Both custom drivers are wired into the build the same way as KSN-1's: `config/zephyr/module.yml` (registers `config/` as a Zephyr module) + `config/CMakeLists.txt` (`target_sources(app PRIVATE src/ksn2_conn_status.c)`) + the source file itself under `config/src/`.

### Keymap (`config/ksn_2.keymap`)

Three layers, same structure as KSN-1's:

- **`default_layer`** — Windows base layer. Left encoder = volume up/down. Left column 8 (`RC(x,8)`) is intentionally `&none` — that column isn't physically wired on this half.
- **`func_layer`** (momentary, `&mo 1`) — Bluetooth profile select (0–4) and output toggle, backlight inc/dec on the encoder, and a toggle (`&tog 2`) into `mac_layer`.
- **`mac_layer`** — Mac modifier ordering and Mac media/brightness keys in place of the F-row/volume keys, same overall shape as `default_layer` otherwise.

## Building

CI (`.github/workflows/`) uses ZMK's standard reusable workflow (`zmkfirmware/zmk/.github/workflows/build-user-config.yml@main`) — no custom build logic. `build.yaml` defines three targets:

```yaml
include:
  - board: nice_nano//zmk
    shield: ksn_2_left
    snippet: zmk-usb-logging
  - board: nice_nano//zmk
    shield: ksn_2_right
    snippet: zmk-usb-logging
  - board: nice_nano//zmk
    shield: settings_reset
    artifact-name: settings_reset
```

Pushing to this repo builds `ksn_2_left`, `ksn_2_right`, and a `settings_reset` firmware as GitHub Actions artifacts.

To build locally with `west`:

```sh
west init -l config
west update
west build -p -b nice_nano_v2 -- -DSHIELD=ksn_2_left -DZMK_EXTRA_MODULES=$(pwd)/config
west build -p -b nice_nano_v2 -- -DSHIELD=ksn_2_right -DZMK_EXTRA_MODULES=$(pwd)/config
```

## Flashing

Double-tap reset on the nice!nano to enter its UF2 bootloader, then drag the matching `.uf2` (left firmware to the left/central half, right to the right/peripheral half) onto the mounted `NICENANO` drive. The two halves run different firmware images — only the left build has `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y`, the encoder, and both LED drivers.

## Re-pairing / clearing Bluetooth bonds

Flash the `settings_reset` artifact to a half to wipe its stored BLE bonds, then reflash that half's normal firmware and re-pair.

## Notable fixed issues (for future reference)

- **Random/ghost keypresses**: an earlier version of `ksn_2.dtsi` had the GPIO pull direction backwards for `col2row` wiring — pull-down was on the *column* (output/drive) pins instead of the *row* (input/sensing) pins, leaving the actual sensing pins floating. Fixed by moving `GPIO_PULL_DOWN` onto the row pins and leaving the column pins with no pull flag (see the table above / `ksn_2.dtsi`). If ghost input ever reappears on either half, check this first.
- **"Paired but not connected" (right/peripheral half)**: same root cause as KSN-1 — the peripheral's `kscan0` GPIOs weren't registered as a wakeup source, so a GPIO interrupt couldn't bring the MCU out of low-power mode. Fixed with `wakeup-source;` on `kscan0` in the shared `ksn_2.dtsi`, plus `CONFIG_PM_DEVICE=y` on the central (left) `.conf`.
- **Peripheral battery level silently dropped**: `CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS` is an unbounded int Kconfig option that defaults to 0 if never set explicitly, and ZMK's `central_bas_proxy.c` silently discards any peripheral battery event whose index is `>=` that value. With exactly one peripheral (index 0), leaving it unset meant every peripheral battery update was dropped with no error. Fixed by setting `CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS=1` explicitly on the central (left) `.conf`.
- **BLE status LED going stale**: see the "LED drivers" section above — the `zmk-poor-mans-led-indicator` widget only reacted to profile-switch events and could get stuck showing a connected state after an actual disconnect. Replaced with the custom `ksn2_conn_status.c` driver, which polls live connection state.
- **Debugging tip**: don't set `CONFIG_LOG_DEFAULT_LEVEL` by hand to chase a specific symptom — it's a *global* Zephyr log level and will also enable debug logging on unrelated low-level modules (e.g. the USB driver), which can flood the log buffer and drop the messages you actually want. Use the `zmk-usb-logging` build snippet (already in `build.yaml`) instead.
