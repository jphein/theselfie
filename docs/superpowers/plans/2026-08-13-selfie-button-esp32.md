# Selfie Button ESPHome Classic-HID Host Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pair a Bluetooth-Classic selfie shutter button to an ESP32 (WROOM-32) running ESPHome, exposing press events and connection state in Home Assistant.

**Architecture:** A custom ESPHome external component (`selfie_button`) owns the whole Bluedroid BR/EDR stack (no BLE components on this node). It uses the raw Bluedroid HID-host API (`esp_hidh_api.h`, part of the always-built `bt` component — deliberately NOT the `esp_hid` wrapper component, to avoid a build-graph dependency question). BT callbacks run in the BT task and only touch a FreeRTOS queue + atomics; all entity publishing happens in `loop()`.

**Tech Stack:** ESPHome (esp-idf framework), Bluedroid Classic (GAP + HID Host), ESPHome native API to HA. Local compile via pipx `esphome` from `~/Projects/theselfie/`.

**Verification model (per JP's CLAUDE.md):** no host test framework for firmware — each task's gate is `esphome compile` plus staged on-device checks (boot logs, pairing logs, HA entities). Physical steps (plugging in the board, pressing the button) are called out explicitly.

---

### Task 1: Scaffold — YAML + component skeleton that compiles

**Files:**
- Create: `selfie-button-host.yaml`
- Create: `components/selfie_button/__init__.py`
- Create: `components/selfie_button/selfie_button.h`
- Create: `components/selfie_button/selfie_button.cpp`
- Create: `secrets.yaml` (gitignored — copied from HA)

- [ ] **Step 1.1: Fetch fleet secrets**

```bash
ssh <user>@<ha-host> "cat /config/esphome/secrets.yaml" > ~/Projects/theselfie/secrets.yaml
grep -c . ~/Projects/theselfie/secrets.yaml   # expect >0 lines
```
Check which keys exist (`wifi_ssid`, `wifi_password`, OTA/api keys); adjust YAML key names in Step 1.2 to match the fleet convention. Generate a fresh API encryption key if the fleet uses per-device keys: `openssl rand -base64 32`.

- [ ] **Step 1.2: Write `selfie-button-host.yaml`**

```yaml
esphome:
  name: selfie-button-host
  friendly_name: Selfie Button Host

esp32:
  board: esp32dev
  framework:
    type: esp-idf

external_components:
  - source:
      type: local
      path: components

logger:
  level: DEBUG

api:

ota:
  - platform: esphome

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

selfie_button:
  connected:
    name: "Connected"
  events:
    name: "Button"
  forget_bond:
    name: "Forget Bond"
```
(Add `api.encryption.key` / OTA password to match fleet convention found in Step 1.1.)

- [ ] **Step 1.3: Write `components/selfie_button/__init__.py`**

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, button, event
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.const import CONF_ID, DEVICE_CLASS_CONNECTIVITY

CODEOWNERS = ["@jp"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["binary_sensor", "button", "event"]

CONF_CONNECTED = "connected"
CONF_EVENTS = "events"
CONF_FORGET_BOND = "forget_bond"

selfie_ns = cg.esphome_ns.namespace("selfie_button")
SelfieButton = selfie_ns.class_("SelfieButton", cg.Component)
SelfieEvent = selfie_ns.class_("SelfieEvent", event.Event)
ForgetBondButton = selfie_ns.class_("ForgetBondButton", button.Button)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SelfieButton),
        cv.Optional(CONF_CONNECTED): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY
        ),
        cv.Optional(CONF_EVENTS): event.event_schema(SelfieEvent),
        cv.Optional(CONF_FORGET_BOND): button.button_schema(ForgetBondButton),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if conf := config.get(CONF_CONNECTED):
        sens = await binary_sensor.new_binary_sensor(conf)
        cg.add(var.set_connected_sensor(sens))
    if conf := config.get(CONF_EVENTS):
        ev = cg.new_Pvariable(conf[CONF_ID])
        await event.register_event(ev, conf, event_types=["press", "long_press"])
        cg.add(var.set_event(ev))
    if conf := config.get(CONF_FORGET_BOND):
        btn = cg.new_Pvariable(conf[CONF_ID])
        await button.register_button(btn, conf)
        cg.add(btn.set_parent(var))

    # Bluedroid Classic BR/EDR-only + HID Host
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_CLASSIC_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLE_ENABLED", False)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY", True)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BTDM", False)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BLE_ONLY", False)
    add_idf_sdkconfig_option("CONFIG_BT_HID_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_HID_HOST_ENABLED", True)
```

- [ ] **Step 1.4: Write `components/selfie_button/selfie_button.h`**

```cpp
#pragma once
#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/event/event.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <esp_bt.h>
#include <esp_bt_defs.h>
#include <esp_gap_bt_api.h>
#include <esp_hidh_api.h>

namespace esphome {
namespace selfie_button {

class SelfieEvent : public event::Event {};

// Internal notifications from BT-task callbacks to loop()
enum SelfieNote : uint8_t {
  NOTE_CONNECTED = 1,
  NOTE_DISCONNECTED = 2,
  NOTE_PRESS = 3,
  NOTE_LONG_PRESS = 4,
  NOTE_FOUND_DEVICE = 5,   // discovery hit stored in found_addr_
  NOTE_DISC_STOPPED = 6,   // discovery ended (re-arm if still unbonded)
};

class SelfieButton : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_connected_sensor(binary_sensor::BinarySensor *s) { this->connected_sensor_ = s; }
  void set_event(SelfieEvent *e) { this->event_ = e; }
  void forget_bond();

 protected:
  static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  static void hidh_cb(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param);
  void note(SelfieNote n);
  void start_discovery_();
  bool load_bond_();

  binary_sensor::BinarySensor *connected_sensor_{nullptr};
  SelfieEvent *event_{nullptr};

  QueueHandle_t queue_{nullptr};
  esp_bd_addr_t target_addr_{};       // bonded / connecting target
  esp_bd_addr_t found_addr_{};        // discovery result (BT task writes, loop reads)
  bool have_bond_{false};
  bool connected_{false};
  bool discovering_{false};
  bool bt_ready_{false};
  int64_t press_start_us_{0};
  bool report_active_{false};
  uint32_t last_connect_attempt_ms_{0};
};

class ForgetBondButton : public button::Button {
 public:
  void set_parent(SelfieButton *p) { this->parent_ = p; }

 protected:
  void press_action() override { this->parent_->forget_bond(); }
  SelfieButton *parent_{nullptr};
};

extern SelfieButton *global_selfie_button;  // NOLINT

}  // namespace selfie_button
}  // namespace esphome
#endif  // USE_ESP32
```

- [ ] **Step 1.5: Write skeleton `components/selfie_button/selfie_button.cpp`** (compiles; BT init lands in Task 2)

```cpp
#ifdef USE_ESP32
#include "selfie_button.h"
#include "esphome/core/log.h"

namespace esphome {
namespace selfie_button {

static const char *const TAG = "selfie_button";
SelfieButton *global_selfie_button = nullptr;  // NOLINT

void SelfieButton::setup() {
  global_selfie_button = this;
  this->queue_ = xQueueCreate(16, sizeof(uint8_t));
  ESP_LOGI(TAG, "skeleton setup (BT init lands in Task 2)");
}

void SelfieButton::loop() {
  uint8_t n;
  while (this->queue_ != nullptr && xQueueReceive(this->queue_, &n, 0) == pdTRUE) {
    ESP_LOGD(TAG, "note %u", n);
  }
}

void SelfieButton::dump_config() { ESP_LOGCONFIG(TAG, "Selfie Button HID host"); }
void SelfieButton::forget_bond() {}
void SelfieButton::note(SelfieNote n) {
  uint8_t v = n;
  xQueueSendFromISR(this->queue_, &v, nullptr);
}
void SelfieButton::start_discovery_() {}
bool SelfieButton::load_bond_() { return false; }
void SelfieButton::gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {}
void SelfieButton::hidh_cb(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param) {}

}  // namespace selfie_button
}  // namespace esphome
#endif
```

- [ ] **Step 1.6: Compile**

```bash
cd ~/Projects/theselfie && ~/.local/bin/esphome compile selfie-button-host.yaml
```
Expected: full build succeeds. This is the **Stage-1 risk gate**: proves Bluedroid Classic headers (`esp_gap_bt_api.h`, `esp_hidh_api.h`) resolve and the sdkconfig combination (BR_EDR_ONLY + HID host + no BLE) builds inside ESPHome. If `esp_hidh_api.h` is missing or Kconfig conflicts appear, fix here before proceeding (fallback: vendor the IDF `esp_hid` component or adjust sdkconfig names to the pinned IDF version).

- [ ] **Step 1.7: Commit**

```bash
git add selfie-button-host.yaml components/
git commit -m "feat: scaffold selfie_button ESPHome component (compiles, BT stack config)"
```

---

### Task 2: BT Classic stack bring-up (Stage-1 on-device gate)

**Files:**
- Modify: `components/selfie_button/selfie_button.cpp` (replace `setup()`)

- [ ] **Step 2.1: Replace `setup()` with full BT init and add includes**

Add includes at top of the file after `selfie_button.h`:
```cpp
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_timer.h>
#include <cstring>
```

Replace `setup()`:
```cpp
void SelfieButton::setup() {
  global_selfie_button = this;
  this->queue_ = xQueueCreate(16, sizeof(uint8_t));

  esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_bt_controller_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "controller_init failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "controller_enable failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  if (esp_bluedroid_init() != ESP_OK || esp_bluedroid_enable() != ESP_OK) {
    ESP_LOGE(TAG, "bluedroid init/enable failed");
    this->mark_failed();
    return;
  }

  esp_bt_gap_register_callback(SelfieButton::gap_cb);
  esp_bt_hid_host_register_callback(SelfieButton::hidh_cb);
  esp_bt_hid_host_init();

  esp_bt_dev_set_device_name("selfie-button-host");

  // SSP "just works": no display, no keyboard
  esp_bt_io_cap_t io_cap = ESP_BT_IO_CAP_NONE;
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &io_cap, sizeof(io_cap));

  // Connectable so the sleeping button can re-page us; never discoverable
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

  this->bt_ready_ = true;
  ESP_LOGI(TAG, "BT Classic HID host ready");

  if (this->load_bond_()) {
    ESP_LOGI(TAG, "bonded device found, paging it");
    esp_bt_hid_host_connect(this->target_addr_);
    this->last_connect_attempt_ms_ = millis();
  } else {
    ESP_LOGI(TAG, "no bond stored — starting discovery (make the button blink)");
    this->start_discovery_();
  }
}
```

Replace `load_bond_()`:
```cpp
bool SelfieButton::load_bond_() {
  int num = esp_bt_gap_get_bond_device_num();
  if (num <= 0)
    return false;
  esp_bd_addr_t list[4];
  int want = std::min(num, 4);
  if (esp_bt_gap_get_bond_device_list(&want, list) != ESP_OK || want <= 0)
    return false;
  std::memcpy(this->target_addr_, list[0], sizeof(esp_bd_addr_t));
  this->have_bond_ = true;
  return true;
}
```

Replace `start_discovery_()`:
```cpp
void SelfieButton::start_discovery_() {
  if (this->discovering_ || !this->bt_ready_)
    return;
  esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
  if (err == ESP_OK) {
    this->discovering_ = true;
    ESP_LOGI(TAG, "GAP discovery started (10 s window)");
  } else {
    ESP_LOGW(TAG, "start_discovery failed: %s", esp_err_to_name(err));
  }
}
```

- [ ] **Step 2.2: Compile**

```bash
cd ~/Projects/theselfie && ~/.local/bin/esphome compile selfie-button-host.yaml
```
Expected: success. Common fixups at this step: exact IDF enum/struct names for the pinned IDF version (`esp_bt_gap_set_security_param` signature, `ESP_BT_SP_IOCAP_MODE`).

- [ ] **Step 2.3: Flash over USB** *(physical: ESP32 plugged into katana)*

```bash
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null   # if empty, ask JP to plug the board in
~/.local/bin/esphome upload selfie-button-host.yaml --device /dev/ttyUSB0
~/.local/bin/esphome logs selfie-button-host.yaml --device /dev/ttyUSB0
```
Expected boot log: `BT Classic HID host ready`, WiFi connected, API up, and `no bond stored — starting discovery`. This retires the approach-B coexistence risk on hardware.

- [ ] **Step 2.4: Commit**

```bash
git add components/selfie_button/selfie_button.cpp
git commit -m "feat: Bluedroid BR/EDR bring-up — controller, GAP, HID host, SSP just-works"
```

---

### Task 3: Discovery → auto-pair → bond persistence

**Files:**
- Modify: `components/selfie_button/selfie_button.cpp` (replace `gap_cb` and part of `loop()`)

- [ ] **Step 3.1: Implement `gap_cb`**

```cpp
void SelfieButton::gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  auto *self = global_selfie_button;
  if (self == nullptr)
    return;
  switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
      for (int i = 0; i < param->disc_res.num_prop; i++) {
        auto &prop = param->disc_res.prop[i];
        if (prop.type != ESP_BT_GAP_DEV_PROP_COD)
          continue;
        uint32_t cod = *(uint32_t *) prop.val;
        if (esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_PERIPHERAL) {
          std::memcpy(self->found_addr_, param->disc_res.bda, sizeof(esp_bd_addr_t));
          self->note(NOTE_FOUND_DEVICE);
        }
      }
      break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
        self->discovering_ = false;
        self->note(NOTE_DISC_STOPPED);
      }
      break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
      ESP_LOGI(TAG, "auth complete, status=%d", param->auth_cmpl.stat);
      break;
    case ESP_BT_GAP_CFM_REQ_EVT:
      // Just-works numeric confirm — button has no display, accept
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      break;
    default:
      break;
  }
}
```

- [ ] **Step 3.2: Extend `loop()` with the note handler**

```cpp
void SelfieButton::loop() {
  uint8_t n;
  while (this->queue_ != nullptr && xQueueReceive(this->queue_, &n, 0) == pdTRUE) {
    switch (n) {
      case NOTE_FOUND_DEVICE: {
        if (this->connected_ || this->have_bond_)
          break;
        std::memcpy(this->target_addr_, this->found_addr_, sizeof(esp_bd_addr_t));
        ESP_LOGI(TAG, "HID peripheral found %02x:%02x:%02x:%02x:%02x:%02x — cancelling discovery, connecting",
                 this->target_addr_[0], this->target_addr_[1], this->target_addr_[2],
                 this->target_addr_[3], this->target_addr_[4], this->target_addr_[5]);
        esp_bt_gap_cancel_discovery();
        esp_bt_hid_host_connect(this->target_addr_);
        this->last_connect_attempt_ms_ = millis();
        break;
      }
      case NOTE_DISC_STOPPED:
        if (!this->have_bond_ && !this->connected_)
          this->start_discovery_();  // re-arm until paired
        break;
      case NOTE_CONNECTED:
        this->connected_ = true;
        this->have_bond_ = this->load_bond_() || this->have_bond_;
        if (this->connected_sensor_ != nullptr)
          this->connected_sensor_->publish_state(true);
        break;
      case NOTE_DISCONNECTED:
        this->connected_ = false;
        if (this->connected_sensor_ != nullptr)
          this->connected_sensor_->publish_state(false);
        break;
      case NOTE_PRESS:
        if (this->event_ != nullptr)
          this->event_->trigger("press");
        break;
      case NOTE_LONG_PRESS:
        if (this->event_ != nullptr)
          this->event_->trigger("long_press");
        break;
      default:
        break;
    }
  }
  // Belt-and-braces reconnect: page the bonded button every 60 s while disconnected
  if (this->bt_ready_ && this->have_bond_ && !this->connected_ &&
      millis() - this->last_connect_attempt_ms_ > 60000) {
    ESP_LOGD(TAG, "retry paging bonded button");
    esp_bt_hid_host_connect(this->target_addr_);
    this->last_connect_attempt_ms_ = millis();
  }
}
```

- [ ] **Step 3.3: Compile, flash, pair on-device** *(physical: JP long-presses the button until blinking)*

```bash
~/.local/bin/esphome run selfie-button-host.yaml --device /dev/ttyUSB0
```
Expected in logs: `HID peripheral found …`, `auth complete, status=0`, HIDH open (Task 4 wires the log), and after reboot: `bonded device found, paging it` (bond persisted in NVS).

- [ ] **Step 3.4: Commit**

```bash
git add components/selfie_button/selfie_button.cpp
git commit -m "feat: GAP discovery, COD-filtered auto-pair, bond persistence, reconnect loop"
```

---

### Task 4: HID input → events + connected sensor

**Files:**
- Modify: `components/selfie_button/selfie_button.cpp` (replace `hidh_cb`)

- [ ] **Step 4.1: Implement `hidh_cb`**

```cpp
void SelfieButton::hidh_cb(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param) {
  auto *self = global_selfie_button;
  if (self == nullptr)
    return;
  switch (event) {
    case ESP_HIDH_INIT_EVT:
      ESP_LOGI(TAG, "HIDH initialized, status=%d", param->init.status);
      break;
    case ESP_HIDH_OPEN_EVT:
      if (param->open.status == ESP_HIDH_OK &&
          param->open.conn_status == ESP_HIDH_CONN_STATE_CONNECTED) {
        ESP_LOGI(TAG, "HID device connected");
        self->note(NOTE_CONNECTED);
      } else {
        ESP_LOGW(TAG, "HIDH open failed status=%d conn=%d", param->open.status,
                 param->open.conn_status);
      }
      break;
    case ESP_HIDH_CLOSE_EVT:
      ESP_LOGI(TAG, "HID device disconnected");
      // If it vanished mid-press (sleep), close out as a short press
      if (self->report_active_) {
        self->report_active_ = false;
        self->note(NOTE_PRESS);
      }
      self->note(NOTE_DISCONNECTED);
      break;
    case ESP_HIDH_DATA_IND_EVT: {
      auto &d = param->data_ind;
      // Raw report visibility while characterizing the button
      ESP_LOG_BUFFER_HEX_LEVEL(TAG, d.data, d.len, ESP_LOG_DEBUG);
      // Active = any nonzero payload byte (skip byte 0 when it looks like a report ID)
      bool active = false;
      int start = (d.len > 1) ? 1 : 0;
      for (int i = start; i < d.len; i++) {
        if (d.data[i] != 0) {
          active = true;
          break;
        }
      }
      if (active && !self->report_active_) {
        self->report_active_ = true;
        self->press_start_us_ = esp_timer_get_time();
      } else if (!active && self->report_active_) {
        self->report_active_ = false;
        int64_t held_ms = (esp_timer_get_time() - self->press_start_us_) / 1000;
        self->note(held_ms >= 600 ? NOTE_LONG_PRESS : NOTE_PRESS);
      }
      break;
    }
    default:
      break;
  }
}
```

- [ ] **Step 4.2: Compile + flash, characterize reports** *(physical: JP presses the button, short and long)*

```bash
~/.local/bin/esphome run selfie-button-host.yaml --device /dev/ttyUSB0
```
Expected: DEBUG hex dumps of input reports on press/release; `press`/`long_press` events visible in the log and in HA (`event.selfie_button_host_button`). **Adjust the active-report heuristic here if the real reports disagree** (e.g., release report is a different length rather than zeros) — this step exists precisely to ground the parser in observed bytes.

- [ ] **Step 4.3: Verify in HA**

Developer Tools → States: `event.` entity updates on press; `binary_sensor.…_connected` is `on`. Let the button sleep (~minutes idle), then press: connected flips off on HIDH close, then on again on wake, and the press event still arrives (first press after sleep may cost ~1–2 s).

- [ ] **Step 4.4: Commit**

```bash
git add components/selfie_button/selfie_button.cpp
git commit -m "feat: HID input parsing — press/long_press events, connected sensor"
```

---

### Task 5: Forget-bond + polish

**Files:**
- Modify: `components/selfie_button/selfie_button.cpp` (replace `forget_bond()` and `dump_config()`)
- Create: `README.md`

- [ ] **Step 5.1: Implement `forget_bond()` and `dump_config()`**

```cpp
void SelfieButton::forget_bond() {
  ESP_LOGW(TAG, "forgetting bond(s) and restarting discovery");
  if (this->connected_)
    esp_bt_hid_host_disconnect(this->target_addr_);
  int num = esp_bt_gap_get_bond_device_num();
  if (num > 0) {
    esp_bd_addr_t list[4];
    int want = std::min(num, 4);
    if (esp_bt_gap_get_bond_device_list(&want, list) == ESP_OK) {
      for (int i = 0; i < want; i++)
        esp_bt_gap_remove_bond_device(list[i]);
    }
  }
  this->have_bond_ = false;
  this->connected_ = false;
  if (this->connected_sensor_ != nullptr)
    this->connected_sensor_->publish_state(false);
  this->start_discovery_();
}

void SelfieButton::dump_config() {
  ESP_LOGCONFIG(TAG, "Selfie Button HID host:");
  ESP_LOGCONFIG(TAG, "  BT ready: %s", YESNO(this->bt_ready_));
  ESP_LOGCONFIG(TAG, "  Bonded: %s", YESNO(this->have_bond_));
  ESP_LOGCONFIG(TAG, "  Connected: %s", YESNO(this->connected_));
}
```

- [ ] **Step 5.2: Write `README.md`** — what the project is, the Classic-vs-BLE finding (Bermuda can't track BR/EDR), pairing procedure (blink + boot unbonded / Forget Bond button), entity list, local build command.

- [ ] **Step 5.3: Full verification pass**

```bash
~/.local/bin/esphome run selfie-button-host.yaml --device /dev/ttyUSB0
```
- Press → `press` event in HA; hold ≥600 ms → `long_press`.
- `button.…_forget_bond` in HA → logs show bonds removed + discovery restarted; re-pair works.
- Reboot ESP32 → reconnects to bond without re-pairing.

- [ ] **Step 5.4: Commit**

```bash
git add components/selfie_button/selfie_button.cpp README.md
git commit -m "feat: forget-bond button, dump_config, README"
```

---

## Self-review notes

- **Spec coverage:** entities (Task 1/4/5), sdkconfig + no-BLE isolation (Task 1), pairing/bond/reconnect (Task 3), error visibility (Tasks 3–4), staged testing (2.3/3.3/4.2–4.3/5.3). Battery observation is post-plan (multi-day).
- **Known intentional deviation:** none from spec; press classification at release per spec.
- **Type consistency:** all cross-task references (`note()`, `SelfieNote`, `target_addr_`, `load_bond_()`) defined in Task 1 header.
- **Reality check:** exact Bluedroid enum/field names (`ESP_HIDH_OK`, `data_ind`, `disc_st_chg`) may differ slightly in the pinned IDF — compile steps are where they get trued up; that is expected iteration, not plan gaps.
