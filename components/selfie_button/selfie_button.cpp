#ifdef USE_ESP32
#include "selfie_button.h"
#include "esphome/core/hal.h"
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

  // SSP "just works": we have no display and no keyboard
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
    ESP_LOGI(TAG, "no bond stored - starting discovery (make the button blink)");
    this->start_discovery_();
  }
}

void SelfieButton::loop() {
  uint8_t n;
  while (this->queue_ != nullptr && xQueueReceive(this->queue_, &n, 0) == pdTRUE) {
    switch (n) {
      case NOTE_FOUND_DEVICE: {
        if (this->connected_ || this->have_bond_)
          break;
        std::memcpy(this->target_addr_, this->found_addr_, sizeof(esp_bd_addr_t));
        ESP_LOGI(TAG, "HID peripheral found %02x:%02x:%02x:%02x:%02x:%02x - connecting",
                 this->target_addr_[0], this->target_addr_[1], this->target_addr_[2], this->target_addr_[3],
                 this->target_addr_[4], this->target_addr_[5]);
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
        ESP_LOGD(TAG, "press");
        if (this->event_ != nullptr)
          this->event_->trigger("press");
        break;
      case NOTE_LONG_PRESS:
        ESP_LOGD(TAG, "long_press");
        if (this->event_ != nullptr)
          this->event_->trigger("long_press");
        break;
      default:
        break;
    }
  }
  // Belt-and-braces: the primary reconnect path is the button paging us on wake,
  // but page the bonded button ourselves every 60 s while disconnected.
  if (this->bt_ready_ && this->have_bond_ && !this->connected_ &&
      millis() - this->last_connect_attempt_ms_ > 60000) {
    ESP_LOGD(TAG, "retry paging bonded button");
    esp_bt_hid_host_connect(this->target_addr_);
    this->last_connect_attempt_ms_ = millis();
  }
}

void SelfieButton::dump_config() {
  ESP_LOGCONFIG(TAG, "Selfie Button HID host:");
  ESP_LOGCONFIG(TAG, "  BT ready: %s", YESNO(this->bt_ready_));
  ESP_LOGCONFIG(TAG, "  Bonded: %s", YESNO(this->have_bond_));
  ESP_LOGCONFIG(TAG, "  Connected: %s", YESNO(this->connected_));
}

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

void SelfieButton::note(SelfieNote n) {
  uint8_t v = n;
  // BT callbacks run in the Bluedroid task (not an ISR)
  xQueueSend(this->queue_, &v, 0);
}

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

void SelfieButton::gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  auto *self = global_selfie_button;
  if (self == nullptr)
    return;
  switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
      if (self->connected_ || self->have_bond_)
        break;
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
      // Just-works numeric confirm - the button has no display, accept
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      break;
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
      break;
    case ESP_HIDH_OPEN_EVT:
      if (param->open.status == ESP_HIDH_OK && param->open.conn_status == ESP_HIDH_CONN_STATE_CONNECTED) {
        ESP_LOGI(TAG, "HID device connected");
        self->note(NOTE_CONNECTED);
      } else {
        ESP_LOGW(TAG, "HIDH open failed status=%d conn=%d", param->open.status, param->open.conn_status);
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

}  // namespace selfie_button
}  // namespace esphome
#endif  // USE_ESP32
