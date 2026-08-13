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

// Notifications from BT-task callbacks to loop(); entity work happens only in loop().
// Queue items are uint16_t: low byte = SelfieNote, high byte = button index.
enum SelfieNote : uint8_t {
  NOTE_CONNECTED = 1,
  NOTE_DISCONNECTED = 2,
  NOTE_PRESS = 3,
  NOTE_FOUND_DEVICE = 5,
  NOTE_DISC_STOPPED = 6,
  NOTE_HIDH_READY = 7,
};

// Order must match BUTTON_EVENT_TYPES in selfie_button.cpp
enum SelfieButtonIndex : uint8_t {
  BTN_TAKE_PHOTO = 0,
  BTN_PLAY_PAUSE = 1,
  BTN_VOLUME_UP = 2,
  BTN_VOLUME_DOWN = 3,
  BTN_SKIP_FORWARD = 4,
  BTN_SKIP_BACK = 5,
  BTN_COUNT = 6,
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
  void note(SelfieNote n, uint8_t btn = 0);
  void start_discovery_();
  bool load_bond_();

  binary_sensor::BinarySensor *connected_sensor_{nullptr};
  SelfieEvent *event_{nullptr};

  QueueHandle_t queue_{nullptr};
  esp_bd_addr_t target_addr_{};
  esp_bd_addr_t found_addr_{};
  bool have_bond_{false};
  bool connected_{false};
  bool discovering_{false};
  bool bt_ready_{false};
  bool hidh_ready_{false};
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
