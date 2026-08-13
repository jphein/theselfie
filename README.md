# theselfie

ESPHome firmware that turns a **Gabba Goods "The Selfie"** Bluetooth remote
(6 buttons: shutter + media controls) into Home Assistant inputs, using an
original ESP32 (WROOM-32) as a Bluetooth **Classic** HID host.

## Why this exists (the finding)

The button is Bluetooth Classic (BR/EDR), not BLE — verified 2026-08-12:
blinking in pairing mode it produced **zero** BLE advertisements on the
ESPHome `bluetooth_proxy` mesh and zero LE sightings from a dual-mode
adapter, yet shows up in a phone's Classic pairing list. ESPHome proxies
forward BLE only, so **Bermuda can never see or track this device**. Full
analysis: `docs/superpowers/specs/2026-08-13-selfie-button-esp32-design.md`.

So instead of a tracker it becomes a button: the ESP32 hosts the HID
connection and exposes presses to HA.

## Hardware

- Original ESP32 (WROOM-32 / `esp32dev`) — the only ESP32 variant with BR/EDR.
  S2/S3/C3/C6 cannot run this.
- The button sleeps aggressively; it wakes on press and re-pages the host
  (first press after sleep costs ~1–2 s).

## Entities

| Entity | Meaning |
|---|---|
| `event.selfie_button_host_button` | one event type per key, fired on press-down: `take_photo`, `play_pause`, `volume_up`, `volume_down`, `skip_forward`, `skip_back` |
| `binary_sensor.selfie_button_host_connected` | HID session up. Off while the remote sleeps — that's normal |
| `button.selfie_button_host_forget_bond` | Drop all Classic bonds and restart pairing discovery |

HID report map (measured 2026-08-13): keyboard report id `0x01` keycode
`0x28` = take_photo; consumer report id `0x03` mask `0x08/0x02/0x04/0x10/0x01`
= play_pause / volume_up / volume_down / skip_forward / skip_back.

**HA side** (in `~/Projects/ha`): dashboard `selfie-remote` + package
`packages/selfie_remote.yaml` — per-button entity dropdowns on the board and a
domain-smart dispatcher automation (media semantics / run / apply / toggle).

## Pairing

1. Flash; on first boot (no bond) the firmware loops GAP discovery.
2. Long-press the button until its LED blinks.
3. It's found by HID Class-of-Device, connected, bonded (SSP just-works).
   Bond survives reboots (Bluedroid NVS). To re-pair, press **Forget Bond**.

## Build & flash

```bash
# secrets.yaml comes from HA:  ssh <user>@<ha-host> "cat /config/esphome/secrets.yaml" > secrets.yaml
esphome run selfie-button-host.yaml --device /dev/ttyUSB0   # USB first flash
esphome run selfie-button-host.yaml                          # OTA afterwards
```

## Design notes

- `components/selfie_button/` owns the whole Bluedroid stack — this node runs
  **no BLE components**, BR/EDR-only controller (`CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY`),
  BLE host compiled out.
- Uses the raw Bluedroid HID host API (`esp_hidh_api.h`), not the `esp_hid`
  wrapper component, to avoid extra build-graph dependencies.
- BT callbacks (Bluedroid task) only push notes onto a FreeRTOS queue;
  all entity publishing happens in `loop()`.
- Pairing security: SSP confirm is only accepted for the exact device found by
  discovery in the current unbonded session; unsolicited pairing attempts are
  rejected, and failed auth removes the half-made bond.

## Serial gotcha (this specific board)

`esphome logs --device /dev/ttyUSB0` holds this CP2102 board in a reset loop
via DTR/RTS (the log stream shows endless truncated `POWERON_RESET` banners /
newline flood). Read the console with a raw reader that parks the handshake
lines instead:

```python
import serial
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=1)
s.dtr = False; s.rts = False
```

Flashing with `esphome upload` (esptool) is unaffected. Network logs via
`esphome logs` (OTA/api, no `--device`) are also fine once WiFi is up.
