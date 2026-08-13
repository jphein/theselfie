#ifdef USE_ESP32
#include "selfie_button.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <esp_bt_device.h>
#include <esp_bt_main.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstring>

namespace esphome {
namespace selfie_button {

static const char *const TAG = "selfie_button";
SelfieButton *global_selfie_button = nullptr;  // NOLINT

// Order must match GABBA_EVENT_TYPES in __init__.py
static const char *const GABBA_EVENT_TYPES[BTN_COUNT] = {
    "take_photo", "play_pause", "volume_up", "volume_down", "skip_forward", "skip_back",
};
// Generic model event names: index 0 = kb_key, 1..8 = consumer_bit_0..7
static const char *const GENERIC_EVENT_TYPES[9] = {
    "kb_key",         "consumer_bit_0", "consumer_bit_1", "consumer_bit_2", "consumer_bit_3",
    "consumer_bit_4", "consumer_bit_5", "consumer_bit_6", "consumer_bit_7",
};

// Gabba consumer-control bitmask (03 00 <mask> 00), verified on-device 2026-08-13
static uint8_t gabba_mask_to_btn(uint8_t mask) {
  switch (mask) {
    case 0x02:
      return BTN_PLAY_PAUSE;
    case 0x08:
      return BTN_VOLUME_UP;
    case 0x10:
      return BTN_VOLUME_DOWN;
    case 0x01:
      return BTN_SKIP_FORWARD;
    case 0x04:
      return BTN_SKIP_BACK;
    default:
      return BTN_COUNT;
  }
}

void SelfieButton::add_device(const std::string &slug, DeviceModel model) {
  DeviceSlot s;
  s.slug = slug;
  s.model = model;
  this->slots_.push_back(s);
}

void SelfieButton::setup() {
  global_selfie_button = this;
  this->queue_ = xQueueCreate(24, sizeof(uint16_t));

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

  esp_bt_io_cap_t io_cap = ESP_BT_IO_CAP_NONE;
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &io_cap, sizeof(io_cap));

  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

  // Restore slot -> MAC assignments from NVS, then adopt any unclaimed
  // Bluedroid bonds into empty slots (covers migration from v1 firmware).
  for (size_t i = 0; i < this->slots_.size(); i++) {
    auto &s = this->slots_[i];
    s.pref = global_preferences->make_preference<BondPref>(fnv1_hash("selfie_bond_" + s.slug));
    BondPref bp{};
    if (s.pref.load(&bp) && bp.valid == 1) {
      std::memcpy(s.addr, bp.mac, 6);
      s.have_bond = true;
      ESP_LOGI(TAG, "slot %u (%s): bond %02x:%02x:%02x:%02x:%02x:%02x from prefs", i, s.slug.c_str(), s.addr[0],
               s.addr[1], s.addr[2], s.addr[3], s.addr[4], s.addr[5]);
    }
  }
  int num = esp_bt_gap_get_bond_device_num();
  if (num > 0) {
    esp_bd_addr_t list[8];
    int want = std::min(num, 8);
    if (esp_bt_gap_get_bond_device_list(&want, list) == ESP_OK) {
      for (int b = 0; b < want; b++) {
        if (this->slot_by_addr_(list[b]) >= 0)
          continue;  // already claimed by a slot
        for (auto &s : this->slots_) {
          if (!s.have_bond) {
            std::memcpy(s.addr, list[b], 6);
            s.have_bond = true;
            ESP_LOGI(TAG, "adopted unclaimed bond into slot '%s'", s.slug.c_str());
            break;
          }
        }
      }
    }
  }
  // Persist adoptions (safe to call from setup; loop-context equivalent)
  for (size_t i = 0; i < this->slots_.size(); i++) {
    if (this->slots_[i].have_bond)
      this->save_bond_pref_(i);
  }

  this->bt_ready_ = true;
  // Honest initial state: disconnected until a HID session actually opens
  for (auto &s : this->slots_) {
    if (s.connected_sensor != nullptr)
      s.connected_sensor->publish_state(false);
  }
  ESP_LOGI(TAG, "BT Classic HID host ready (%u slots)", this->slots_.size());

  bool any_bond = false;
  for (auto &s : this->slots_)
    any_bond |= s.have_bond;
  if (!any_bond) {
    ESP_LOGI(TAG, "no bonds stored - discovery armed for slot 0 (make the remote blink)");
    this->pairing_slot_ = 0;
    this->start_discovery_();
  }
  // Bonded slots get paged from loop() once NOTE_HIDH_READY arrives.
}

void SelfieButton::loop() {
  uint16_t item;
  while (this->queue_ != nullptr && xQueueReceive(this->queue_, &item, 0) == pdTRUE) {
    uint8_t n = item & 0x0F;
    uint8_t slot = (item >> 4) & 0x0F;
    uint8_t code = item >> 8;
    switch (n) {
      case NOTE_FOUND_DEVICE: {
        if (this->pairing_slot_ < 0 || this->pairing_slot_ >= (int) this->slots_.size())
          break;
        auto &s = this->slots_[this->pairing_slot_];
        if (s.have_bond)
          break;
        std::memcpy(s.addr, this->found_addr_, 6);
        ESP_LOGI(TAG, "HID peripheral found %02x:%02x:%02x:%02x:%02x:%02x - pairing into slot '%s'", s.addr[0],
                 s.addr[1], s.addr[2], s.addr[3], s.addr[4], s.addr[5], s.slug.c_str());
        esp_bt_gap_cancel_discovery();
        esp_bt_hid_host_connect(s.addr);
        s.last_connect_attempt_ms = millis();
        break;
      }
      case NOTE_DISC_STOPPED:
        // Re-arm only while a pairing window is open and unfulfilled
        if (this->pairing_slot_ >= 0 && !this->slots_[this->pairing_slot_].have_bond)
          this->start_discovery_();
        break;
      case NOTE_HIDH_READY:
        this->hidh_ready_ = true;
        for (size_t i = 0; i < this->slots_.size(); i++) {
          auto &s = this->slots_[i];
          if (s.have_bond && !s.connected) {
            ESP_LOGI(TAG, "HIDH up - paging '%s'", s.slug.c_str());
            esp_bt_hid_host_connect(s.addr);
            s.last_connect_attempt_ms = millis() + (uint32_t) i * 5000;  // stagger
          }
        }
        break;
      case NOTE_ADOPTED: {
        auto &s = this->slots_[slot];
        s.have_bond = true;
        this->save_bond_pref_(slot);
        if (this->pairing_slot_ == (int) slot)
          this->pairing_slot_ = -1;
        ESP_LOGI(TAG, "slot '%s' bonded and persisted", s.slug.c_str());
        break;
      }
      case NOTE_CONNECTED: {
        auto &s = this->slots_[slot];
        s.connected = true;
        if (s.connected_sensor != nullptr)
          s.connected_sensor->publish_state(true);
        break;
      }
      case NOTE_DISCONNECTED: {
        auto &s = this->slots_[slot];
        s.connected = false;
        if (s.connected_sensor != nullptr)
          s.connected_sensor->publish_state(false);
        break;
      }
      case NOTE_PRESS: {
        auto &s = this->slots_[slot];
        const char *ev = nullptr;
        if (s.model == MODEL_GABBA && code < BTN_COUNT) {
          ev = GABBA_EVENT_TYPES[code];
        } else if (s.model == MODEL_GENERIC && code < 9) {
          ev = GENERIC_EVENT_TYPES[code];
        }
        if (ev != nullptr) {
          ESP_LOGD(TAG, "'%s' press: %s", s.slug.c_str(), ev);
          if (s.event != nullptr)
            s.event->trigger(ev);
        }
        break;
      }
      default:
        break;
    }
  }
  // Per-slot belt-and-braces reconnect paging, staggered
  if (this->bt_ready_ && this->hidh_ready_) {
    for (auto &s : this->slots_) {
      if (s.have_bond && !s.connected && millis() - s.last_connect_attempt_ms > 60000) {
        ESP_LOGD(TAG, "retry paging '%s'", s.slug.c_str());
        esp_bt_hid_host_connect(s.addr);
        s.last_connect_attempt_ms = millis();
      }
    }
  }
}

void SelfieButton::dump_config() {
  ESP_LOGCONFIG(TAG, "Selfie Button HID host:");
  ESP_LOGCONFIG(TAG, "  BT ready: %s", YESNO(this->bt_ready_));
  for (auto &s : this->slots_) {
    ESP_LOGCONFIG(TAG, "  slot '%s': model=%s bonded=%s connected=%s", s.slug.c_str(),
                  s.model == MODEL_GABBA ? "gabba_selfie" : "generic", YESNO(s.have_bond), YESNO(s.connected));
  }
}

void SelfieButton::pair_reset(int slot) {
  if (slot < 0 || slot >= (int) this->slots_.size())
    return;
  auto &s = this->slots_[slot];
  if (s.have_bond) {
    ESP_LOGW(TAG, "slot '%s': forgetting bond, reopening pairing", s.slug.c_str());
    if (s.connected)
      esp_bt_hid_host_disconnect(s.addr);
    esp_bt_gap_remove_bond_device(s.addr);
    s.have_bond = false;
    s.connected = false;
    s.handle = -1;
    if (s.connected_sensor != nullptr)
      s.connected_sensor->publish_state(false);
    BondPref bp{};
    s.pref.save(&bp);
  } else {
    ESP_LOGI(TAG, "slot '%s': pairing window opened", s.slug.c_str());
  }
  this->pairing_slot_ = slot;
  this->start_discovery_();
}

void SelfieButton::note(SelfieNote n, uint8_t slot, uint8_t code) {
  uint16_t v = (uint16_t)(n & 0x0F) | ((uint16_t)(slot & 0x0F) << 4) | ((uint16_t) code << 8);
  // BT callbacks run in the Bluedroid task (not an ISR)
  xQueueSend(this->queue_, &v, 0);
}

void SelfieButton::start_discovery_() {
  if (this->discovering_ || !this->bt_ready_)
    return;
  esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
  if (err == ESP_OK) {
    this->discovering_ = true;
    ESP_LOGI(TAG, "GAP discovery started (10 s window, pairing slot %d)", this->pairing_slot_);
  } else {
    ESP_LOGW(TAG, "start_discovery failed: %s", esp_err_to_name(err));
  }
}

int SelfieButton::slot_by_addr_(const uint8_t *bda) {
  for (size_t i = 0; i < this->slots_.size(); i++) {
    if (this->slots_[i].have_bond && std::memcmp(this->slots_[i].addr, bda, 6) == 0)
      return (int) i;
  }
  return -1;
}

int SelfieButton::slot_by_handle_(int handle) {
  for (size_t i = 0; i < this->slots_.size(); i++) {
    if (this->slots_[i].handle == handle)
      return (int) i;
  }
  return -1;
}

void SelfieButton::save_bond_pref_(int slot) {
  auto &s = this->slots_[slot];
  BondPref bp{};
  std::memcpy(bp.mac, s.addr, 6);
  bp.valid = 1;
  s.pref.save(&bp);
}

void SelfieButton::parse_report_(int slot, const uint8_t *data, uint16_t len) {
  if (slot < 0 || len < 1)
    return;
  auto &s = this->slots_[slot];
  bool active = false;
  uint8_t code = 0xFF;

  if (data[0] == 0x01 && len >= 4) {  // keyboard report
    for (int i = 1; i < len; i++) {
      if (data[i] != 0) {
        active = true;
        break;
      }
    }
    s.last_kb_us = esp_timer_get_time();
    code = (s.model == MODEL_GABBA) ? BTN_TAKE_PHOTO : 0;  // generic: kb_key
  } else if (data[0] == 0x03 && len >= 3) {  // consumer report
    uint8_t mask = data[2];
    active = mask != 0;
    if (s.model == MODEL_GABBA) {
      code = gabba_mask_to_btn(mask);
      // take_photo echoes consumer vol+ (0x08, the iOS shutter) - drop the echo
      if (active && mask == 0x08 && esp_timer_get_time() - s.last_kb_us < 500000) {
        ESP_LOGD(TAG, "'%s': suppressing vol+ echo of take_photo", s.slug.c_str());
        return;
      }
    } else {
      // generic: lowest set bit -> consumer_bit_N (event index N+1)
      for (uint8_t bit = 0; bit < 8; bit++) {
        if (mask & (1 << bit)) {
          code = bit + 1;
          break;
        }
      }
    }
  } else {
    char hex[3 * 16 + 1] = {0};
    int n = len < 16 ? len : 16;
    for (int i = 0; i < n; i++)
      snprintf(hex + i * 3, 4, "%02x ", data[i]);
    ESP_LOGW(TAG, "'%s': unrecognized report len=%d: %s", s.slug.c_str(), len, hex);
    return;
  }

  if (active && !s.report_active) {
    s.report_active = true;
    if (code != 0xFF) {
      this->note(NOTE_PRESS, (uint8_t) slot, code);
    } else {
      ESP_LOGW(TAG, "'%s': unknown button code in report id=%#04x", s.slug.c_str(), data[0]);
    }
  } else if (!active && s.report_active) {
    s.report_active = false;
  }
}

void SelfieButton::gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  auto *self = global_selfie_button;
  if (self == nullptr)
    return;
  switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
      if (self->pairing_slot_ < 0)
        break;
      for (int i = 0; i < param->disc_res.num_prop; i++) {
        auto &prop = param->disc_res.prop[i];
        if (prop.type != ESP_BT_GAP_DEV_PROP_COD)
          continue;
        uint32_t cod = *(uint32_t *) prop.val;
        if (esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_PERIPHERAL &&
            self->slot_by_addr_(param->disc_res.bda) < 0) {
          std::memcpy(self->found_addr_, param->disc_res.bda, 6);
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
      if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "auth complete");
      } else {
        ESP_LOGW(TAG, "auth FAILED status=%d - removing bond for that peer", param->auth_cmpl.stat);
        esp_bt_gap_remove_bond_device(param->auth_cmpl.bda);
      }
      break;
    case ESP_BT_GAP_CFM_REQ_EVT: {
      // Accept SSP confirm only for the device found in the currently open
      // pairing window; reject anything unsolicited.
      bool expecting = self->pairing_slot_ >= 0 &&
                       std::memcmp(param->cfm_req.bda, self->slots_[self->pairing_slot_].addr, 6) == 0;
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, expecting);
      if (!expecting)
        ESP_LOGW(TAG, "rejected unsolicited pairing attempt");
      break;
    }
    default:
      break;
  }
}

void SelfieButton::hidh_cb(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param) {
  auto *self = global_selfie_button;
  if (self == nullptr)
    return;
  switch (event) {
    case ESP_HIDH_INIT_EVT:
      ESP_LOGI(TAG, "HIDH initialized, status=%d", param->init.status);
      self->note(NOTE_HIDH_READY);
      break;
    case ESP_HIDH_OPEN_EVT:
      if (param->open.status == ESP_HIDH_OK && param->open.conn_status == ESP_HIDH_CONN_STATE_CONNECTED) {
        int slot = self->slot_by_addr_(param->open.bd_addr);
        if (slot < 0 && self->pairing_slot_ >= 0 &&
            std::memcmp(param->open.bd_addr, self->slots_[self->pairing_slot_].addr, 6) == 0) {
          slot = self->pairing_slot_;
          self->note(NOTE_ADOPTED, (uint8_t) slot);
        }
        if (slot >= 0) {
          self->slots_[slot].handle = param->open.handle;
          ESP_LOGI(TAG, "HID device connected (slot %d, handle %d)", slot, param->open.handle);
          self->note(NOTE_CONNECTED, (uint8_t) slot);
        } else {
          ESP_LOGW(TAG, "open from unknown peer - disconnecting");
          esp_bt_hid_host_disconnect(param->open.bd_addr);
        }
      } else {
        ESP_LOGW(TAG, "HIDH open failed status=%d conn=%d", param->open.status, param->open.conn_status);
      }
      break;
    case ESP_HIDH_CLOSE_EVT: {
      int slot = self->slot_by_handle_(param->close.handle);
      if (slot >= 0) {
        ESP_LOGI(TAG, "HID device disconnected (slot %d)", slot);
        self->slots_[slot].handle = -1;
        self->slots_[slot].report_active = false;
        self->note(NOTE_DISCONNECTED, (uint8_t) slot);
      }
      break;
    }
    case ESP_HIDH_DATA_IND_EVT: {
      auto &d = param->data_ind;
      self->parse_report_(self->slot_by_handle_(d.handle), d.data, d.len);
      break;
    }
    default:
      break;
  }
}

}  // namespace selfie_button
}  // namespace esphome
#endif  // USE_ESP32
