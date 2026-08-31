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

---

# ksn2-firmware (한국어)

KSN-2 스플릿 키보드용 ZMK 펌웨어 설정입니다 — 원조 [KSN-1](https://github.com/Kesaros44/ksn1-firmware) 설계에서 파생된 70% 스플릿 보드이며, 검증된 패턴(커스텀 LED 드라이버, peripheral half의 `wakeup-source` 처리) 다수를 재사용합니다.

* 키보드 관리자: [AJG](https://github.com/Kesaros44)
* 지원 하드웨어: KSN-2 스플릿 키보드, nice!nano v2 (nRF52840), BLE

## 하드웨어

- **MCU:** half마다 nice!nano v2 (nRF52840), 무선(BLE)
- **Central:** **왼쪽** half (오른쪽이 central인 KSN-1과 반대)
- **매트릭스:** half당 6행 x 9열, 합쳐서 6행 x 18열; `diode-direction = "col2row"` — row는 입력, col은 출력
- **인코더:** EC11 인코더 1개, 왼쪽(central) half에만 있음
- **LED (왼쪽/central 전용):** Caps Lock LED와 BLE 연결상태 LED
- **백라이트:** PWM 백라이트(양쪽 half 모두 `P0.06` 핀), 부팅 시 기본 꺼짐, 유휴 시 자동 꺼짐
- **RGB 언더글로우:** 이 보드에는 없음
- **배터리:** 양쪽 half 모두 배터리 보고 활성화; central이 peripheral의 배터리 잔량도 가져와 호스트에 보고

### 핀아웃

Row는 양쪽 half 동일:

| Row | GPIO |
| --- | --- |
| row1 | P1.00 |
| row2 | P0.24 |
| row3 | P0.22 |
| row4 | P0.20 |
| row5 | P0.17 |
| row6 | P0.08 |

Column도 양쪽 half 동일:

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

왼쪽 전용 추가 항목:

| 기능 | GPIO |
| --- | --- |
| 인코더 A / EN1 | P1.15 |
| 인코더 B / EN2 | P1.13 |
| Caps Lock LED | P0.02 |
| BLE 상태 LED / indicator-led | P0.29 |
| 백라이트 PWM | P0.06 |

### LED 드라이버

- **Caps Lock LED** — ZMK 내장 `zmk,indicator-leds` 노드 + `HID_INDICATOR_CAPS_LOCK`으로 구동. 이 보드에서는 이게 **central**(왼쪽) half에 있기 때문에 동작합니다 — 이 ZMK API는 central 전용이라 peripheral에서는 링크에 실패하는데, 이게 정확히 KSN-1의 커스텀 `ksn1_peripheral_indicators.c`가 우회해야 했던 제약입니다(KSN-1은 Caps Lock LED가 peripheral half에 있음).
- **BLE 상태 LED**(`indicator-led`/`ble_led`) — 커스텀 드라이버 `config/src/ksn2_conn_status.c`로 구동: 호스트 연결 시 상시 점등, 아니면 500ms마다 점멸하며, `zmk_ble_active_profile_changed` 이벤트마다 실시간 재확인에 더해 안전망으로 1초 간격 연결상태 폴링도 수행. 이것은 `zmk-poor-mans-led-indicator` 모듈의 `CONFIG_INDICATOR_LED_SHOW_BLE` 위젯(여전히 `west.yml`에는 임포트되어 있지만 `CONFIG_INDICATOR_LED_WIDGET=n`으로 비활성화됨)을 대체한 것입니다 — 그 위젯은 BT 프로필 전환 이벤트에서만 재평가되기 때문에, 그 이벤트가 한 번 지나가고 나면 호스트가 실제로 계속 연결되어 있는지와 무관하게 점멸 시퀀스가 끝난 상태 그대로 LED가 고정되는 문제가 있었습니다. (Nexplit Keypad의 `keypad_indicator.c`에서 이미 검증된 동일한 수정을 그대로 이식.)

두 커스텀 드라이버 모두 KSN-1과 동일한 방식으로 빌드에 연결됩니다: `config/zephyr/module.yml`(`config/`를 Zephyr 모듈로 등록) + `config/CMakeLists.txt`(`target_sources(app PRIVATE src/ksn2_conn_status.c)`) + `config/src/` 아래의 소스 파일.

### 키맵 (`config/ksn_2.keymap`)

KSN-1과 동일한 구조의 3개 레이어:

- **`default_layer`** — Windows 기본 레이어. 왼쪽 인코더 = 볼륨 업/다운. 왼쪽 8번째 열(`RC(x,8)`)은 의도적으로 `&none` — 이 half에는 물리적으로 배선되지 않은 열입니다.
- **`func_layer`** (모멘터리, `&mo 1`) — 블루투스 프로필 선택(0–4) 및 출력 토글, 인코더로 백라이트 증감, `mac_layer`로의 토글(`&tog 2`).
- **`mac_layer`** — Mac 방식 모디파이어 순서와 F행/볼륨 키 대신 Mac 미디어/밝기 키. 그 외 전체적인 구조는 `default_layer`와 동일.

## 빌드

CI(`.github/workflows/`)는 ZMK 표준 재사용 워크플로우(`zmkfirmware/zmk/.github/workflows/build-user-config.yml@main`)를 사용합니다 — 별도 커스텀 빌드 로직 없음. `build.yaml`이 세 개의 타겟을 정의합니다:

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

이 저장소에 push하면 `ksn_2_left`, `ksn_2_right`, `settings_reset` 펌웨어가 GitHub Actions artifact로 빌드됩니다.

`west`로 로컬 빌드하려면:

```sh
west init -l config
west update
west build -p -b nice_nano_v2 -- -DSHIELD=ksn_2_left -DZMK_EXTRA_MODULES=$(pwd)/config
west build -p -b nice_nano_v2 -- -DSHIELD=ksn_2_right -DZMK_EXTRA_MODULES=$(pwd)/config
```

## 플래싱

nice!nano의 리셋 버튼을 더블탭해서 UF2 부트로더로 진입한 뒤, 마운트된 `NICENANO` 드라이브에 해당하는 `.uf2`(왼쪽 펌웨어는 왼쪽/central half에, 오른쪽은 오른쪽/peripheral half에)를 드래그하면 됩니다. 양쪽 half는 서로 다른 펌웨어 이미지를 사용합니다 — 왼쪽 빌드만 `CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y`, 인코더, 두 LED 드라이버를 모두 가짐.

## 재페어링 / 블루투스 본딩 초기화

`settings_reset` artifact를 해당 half에 플래시하면 저장된 BLE 본딩이 초기화됩니다. 그 다음 해당 half에 정상 펌웨어를 다시 플래시하고 재페어링하세요.

## 알려진 해결 이슈 (참고용)

- **랜덤/유령 입력**: 이전 버전의 `ksn_2.dtsi`가 `col2row` 배선에 대한 GPIO 풀 방향을 반대로 설정했습니다 — pull-down이 *row*(입력/센싱) 핀이 아니라 *col*(출력/구동) 핀에 걸려 있어서, 정작 센싱해야 할 핀이 플로팅 상태였습니다. `GPIO_PULL_DOWN`을 row 핀으로 옮기고 col 핀은 풀 플래그 없이 두어 해결했습니다(위 표 / `ksn_2.dtsi` 참고). 어느 쪽 half에서든 유령 입력이 다시 나타나면 이것부터 확인하세요.
- **"페어링됨이지만 연결 안 됨" (오른쪽/peripheral half)**: KSN-1과 동일한 원인 — peripheral의 `kscan0` GPIO가 wakeup source로 등록되지 않아서 GPIO 인터럽트로 MCU를 저전력 모드에서 깨울 수 없었습니다. 공용 `ksn_2.dtsi`의 `kscan0`에 `wakeup-source;`를 추가하고, central(왼쪽) `.conf`에 `CONFIG_PM_DEVICE=y`를 추가해서 해결했습니다.
- **Peripheral 배터리 잔량이 조용히 누락됨**: `CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS`는 명시적으로 설정하지 않으면 기본값이 0인 unbounded int Kconfig 옵션이며, ZMK의 `central_bas_proxy.c`는 인덱스가 이 값 이상인 peripheral 배터리 이벤트를 조용히 버립니다. peripheral이 정확히 1개(인덱스 0)인데 이 값을 설정하지 않으면 모든 peripheral 배터리 업데이트가 에러 없이 버려집니다. central(왼쪽) `.conf`에 `CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS=1`을 명시해서 해결했습니다.
- **BLE 상태 LED가 갱신되지 않고 고정됨**: 위 "LED 드라이버" 섹션 참고 — `zmk-poor-mans-led-indicator` 위젯은 프로필 전환 이벤트에만 반응해서, 실제 연결이 끊긴 후에도 연결된 상태로 표시된 채 고정될 수 있었습니다. 실시간 연결 상태를 폴링하는 커스텀 `ksn2_conn_status.c` 드라이버로 교체했습니다.
- **디버깅 팁**: 특정 증상을 쫓는다고 `CONFIG_LOG_DEFAULT_LEVEL`을 손으로 올리지 마세요 — 이건 *전역* Zephyr 로그 레벨이라, 관련 없는 저수준 모듈(예: USB 드라이버)까지 디버그 로깅이 켜져서 로그 버퍼가 넘치고 정작 보고 싶은 메시지가 밀려날 수 있습니다. 대신 `zmk-usb-logging` 빌드 스니펫(이미 `build.yaml`에 있음)을 사용하세요.
