#pragma once
#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/event/event.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <esp_bt.h>
#include <esp_bt_defs.h>
#include <esp_gap_bt_api.h>
#include <esp_hidh_api.h>

#include <string>
#include <vector>

namespace esphome {
namespace selfie_button {

class SelfieEvent : public event::Event {};

enum DeviceModel : uint8_t {
  MODEL_GABBA = 0,    // Gabba Goods 6-key: named events, vol+ echo suppression
  MODEL_GENERIC = 1,  // unknown remote: kb_key / consumer_bit_N events
};

// Queue item (uint16_t): bits 0-3 note, bits 4-7 slot, bits 8-15 button code
enum SelfieNote : uint8_t {
  NOTE_CONNECTED = 1,
  NOTE_DISCONNECTED = 2,
  NOTE_PRESS = 3,
  NOTE_FOUND_DEVICE = 5,
  NOTE_DISC_STOPPED = 6,
  NOTE_HIDH_READY = 7,
  NOTE_ADOPTED = 8,  // pairing target opened; persist bond MAC for the slot
};

// Gabba button indices (order matches GABBA_EVENT_TYPES in __init__.py)
enum GabbaButton : uint8_t {
  BTN_TAKE_PHOTO = 0,
  BTN_PLAY_PAUSE = 1,
  BTN_VOLUME_UP = 2,
  BTN_VOLUME_DOWN = 3,
  BTN_SKIP_FORWARD = 4,
  BTN_SKIP_BACK = 5,
  BTN_COUNT = 6,
};

struct BondPref {
  uint8_t mac[6];
  uint8_t valid;
} __attribute__((packed));

struct DeviceSlot {
  std::string slug;
  DeviceModel model{MODEL_GENERIC};
  binary_sensor::BinarySensor *connected_sensor{nullptr};
  SelfieEvent *event{nullptr};
  ESPPreferenceObject pref;

  esp_bd_addr_t addr{};
  bool have_bond{false};
  bool connected{false};
  int handle{-1};  // HIDH connection handle while open
  bool report_active{false};
  int64_t last_kb_us{0};
  uint32_t last_connect_attempt_ms{0};
};

class SelfieButton : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void add_device(const std::string &slug, DeviceModel model);
  void set_connected_sensor(int slot, binary_sensor::BinarySensor *s) {
    this->slots_[slot].connected_sensor = s;
  }
  void set_event(int slot, SelfieEvent *e) { this->slots_[slot].event = e; }
  void pair_reset(int slot);

 protected:
  static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  static void hidh_cb(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param);
  void note(SelfieNote n, uint8_t slot = 0, uint8_t code = 0);
  void start_discovery_();
  int slot_by_addr_(const uint8_t *bda);
  int slot_by_handle_(int handle);
  void parse_report_(int slot, const uint8_t *data, uint16_t len);
  void save_bond_pref_(int slot);

  std::vector<DeviceSlot> slots_;
  QueueHandle_t queue_{nullptr};
  esp_bd_addr_t found_addr_{};
  int pairing_slot_{-1};  // slot currently authorized to pair, -1 = none
  bool discovering_{false};
  bool bt_ready_{false};
  bool hidh_ready_{false};
};

class PairButton : public button::Button {
 public:
  void set_parent(SelfieButton *p, int slot) {
    this->parent_ = p;
    this->slot_ = slot;
  }

 protected:
  void press_action() override { this->parent_->pair_reset(this->slot_); }
  SelfieButton *parent_{nullptr};
  int slot_{0};
};

extern SelfieButton *global_selfie_button;  // NOLINT

}  // namespace selfie_button
}  // namespace esphome
#endif  // USE_ESP32
