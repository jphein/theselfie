# theselfie — ESPHome Classic-HID host for the Gabba Goods selfie button

**Date:** 2026-08-13 · **Status:** approved (JP, "go ahead full auto")

## Problem

The Gabba Goods "The Selfie" shutter button is Bluetooth **Classic** (BR/EDR),
verified 2026-08-12: while blinking in pairing mode it produced zero BLE
advertisements on the ESPHome proxy mesh (Laundry proxy hears an iTAG in the
same room at −83 dBm) and zero LE sightings from katana's dual-mode radio, yet
appears in a phone's Classic device list. ESPHome `bluetooth_proxy` forwards
BLE only, so **Bermuda can never track this device**. Repurpose it instead:
pair it to a dedicated ESP32 running as a Classic HID host and surface presses
in Home Assistant.

## Decision

ESPHome **external component** (`selfie_button`) on JP's original ESP32
(WROOM-32 — the only ESP32 variant with BR/EDR). Approach chosen by JP over a
raw ESP-IDF+MQTT firmware; risk is contained by running **no BLE components**
on this node, so the component owns the entire Bluedroid stack (ESPHome only
initializes BT when a BT component requests it).

## Architecture

- Device: `selfie-button-host` (ESP32 WROOM-32), ESPHome, `framework: esp-idf`.
- `components/selfie_button/` external component:
  - `__init__.py` codegen sets Bluedroid sdkconfig via
    `esp32.add_idf_sdkconfig_option`: `CONFIG_BT_ENABLED`,
    `CONFIG_BT_CLASSIC_ENABLED`, `CONFIG_BT_HID_ENABLED`,
    `CONFIG_BT_HID_HOST_ENABLED`, controller mode `BR_EDR_ONLY`
    (BLE disabled — saves RAM, nothing else uses it).
  - C++ hub initializes controller + Bluedroid + `esp_hidh` (Espressif's HID
    host API, per the official `esp_hid_host` example), GAP scan mode
    CONNECTABLE / NON-DISCOVERABLE so the sleeping button can re-page us.
- HA connectivity: ESPHome native API. No MQTT.
- BT callbacks run in BT task context → they only set atomics/queues; all
  entity publishing happens in `loop()`.

## Entities

| Entity | Type | Behavior |
|---|---|---|
| `event.selfie_button` | event (`press`, `long_press`) | nonzero HID input report = press; release >600 ms later ⇒ `long_press`, else `press` |
| `binary_sensor.selfie_button_connected` | connectivity | HIDH OPEN/CLOSE events. Reads off whenever the button sleeps — that is honest, not a bug |
| `button.selfie_forget_bond` | button | `esp_bt_gap_remove_bond_device` + reboot into discovery |

## Pairing & reconnect

1. Boot: `esp_bt_gap_get_bond_device_list`. Bond exists → page it once
   (`esp_hidh_dev_open`), then wait connectable.
2. No bond → Classic GAP discovery filtered by HID Class-of-Device
   (major = peripheral); JP long-presses the button to blink; first HID hit is
   opened + bonded (SSP; bond persists in Bluedroid NVS).
3. Disconnect (button sleeps): stay connectable (primary reconnect path is the
   button paging us on wake) + slow retry page every 60 s as belt-and-braces.
4. First press after sleep costs ~1–2 s reconnect; subsequent presses instant.

## Error handling

- Connected sensor mirrors HIDH open/close → failures visible in HA.
- Discovery timeout re-arms discovery.
- Build risk (main one): `esp_hid` IDF component availability inside
  ESPHome's PlatformIO espidf build — proven/disproven at Stage 1 compile
  before any feature work.

## Repo & workflow

`~/Projects/theselfie/` (git): `components/selfie_button/`,
`selfie-button-host.yaml`, `secrets.yaml` (gitignored, copied from HA per ha
skill). Local compile via pipx `esphome` (5× faster than the HA VM). Flash
over USB first; OTA afterwards. realm-sigil: N/A (no HTTP presence; ESPHome
node, same as the rest of the fleet).

## Test plan

1. **Stage 1 — stack proof:** bare component (init only) compiles and boots;
   WiFi + native API up alongside BR/EDR controller. This retires the
   approach-B risk before feature code.
2. **Stage 2 — pairing:** discovery finds the blinking button, bonds, logs HID
   input reports.
3. **Stage 3 — HA:** event fires on press; connected sensor tracks sleep/wake
   reconnect; multi-day battery observation.

## Out of scope

- Tracking/Bermuda (impossible for BR/EDR — documented above).
- BLE proxy duty on this node (deliberately excluded).
- Multi-button support (YAGNI; one bonded device).
