# ksn2-firmware

ZMK firmware configuration for the KSN-2 split keyboard.

## Hardware

- MCU: nRF52840 Pro Micro compatible controller
- Keyboard type: split, left side central
- Matrix: 6 rows x 9 columns per side, 6 rows x 18 columns total
- Diode direction: col2row
- Encoder: one EC11 encoder on the left side only
- LEDs: left-side connection LED and Caps Lock LED
- Backlight: PWM backlight node is defined, but firmware starts with backlight off
- Battery: ZMK battery reporting is enabled for OS-level battery display

## Pinout

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
| Connection LED / COND | P0.29 |
| Caps Lock LED | P0.02 |
| Backlight PWM | P0.06 |
