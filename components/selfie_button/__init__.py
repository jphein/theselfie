import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, button, event
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.const import CONF_ID, CONF_MODEL, DEVICE_CLASS_CONNECTIVITY

CODEOWNERS = ["@jp"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["binary_sensor", "button", "event"]

CONF_DEVICES = "devices"
CONF_SLUG = "slug"
CONF_CONNECTED = "connected"
CONF_EVENTS = "events"
CONF_PAIR = "pair"

MODEL_GABBA = "gabba_selfie"
MODEL_GENERIC = "generic"

GABBA_EVENT_TYPES = [
    "take_photo",
    "play_pause",
    "volume_up",
    "volume_down",
    "skip_forward",
    "skip_back",
]
GENERIC_EVENT_TYPES = ["kb_key"] + [f"consumer_bit_{i}" for i in range(8)]

selfie_ns = cg.esphome_ns.namespace("selfie_button")
SelfieButton = selfie_ns.class_("SelfieButton", cg.Component)
SelfieEvent = selfie_ns.class_("SelfieEvent", event.Event)
PairButton = selfie_ns.class_("PairButton", button.Button)
DeviceModel = selfie_ns.enum("DeviceModel")

MODELS = {
    MODEL_GABBA: DeviceModel.MODEL_GABBA,
    MODEL_GENERIC: DeviceModel.MODEL_GENERIC,
}

DEVICE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_SLUG): cv.valid_name,
        cv.Optional(CONF_MODEL, default=MODEL_GENERIC): cv.one_of(*MODELS, lower=True),
        cv.Optional(CONF_CONNECTED): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY
        ),
        cv.Optional(CONF_EVENTS): event.event_schema(SelfieEvent),
        cv.Optional(CONF_PAIR): button.button_schema(PairButton),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SelfieButton),
        cv.Required(CONF_DEVICES): cv.All(
            cv.ensure_list(DEVICE_SCHEMA), cv.Length(min=1, max=4)
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    for slot, dev in enumerate(config[CONF_DEVICES]):
        cg.add(var.add_device(dev[CONF_SLUG], MODELS[dev[CONF_MODEL]]))
        if conf := dev.get(CONF_CONNECTED):
            sens = await binary_sensor.new_binary_sensor(conf)
            cg.add(var.set_connected_sensor(slot, sens))
        if conf := dev.get(CONF_EVENTS):
            ev = cg.new_Pvariable(conf[CONF_ID])
            types = (
                GABBA_EVENT_TYPES
                if dev[CONF_MODEL] == MODEL_GABBA
                else GENERIC_EVENT_TYPES
            )
            await event.register_event(ev, conf, event_types=types)
            cg.add(var.set_event(slot, ev))
        if conf := dev.get(CONF_PAIR):
            btn = cg.new_Pvariable(conf[CONF_ID])
            await button.register_button(btn, conf)
            cg.add(btn.set_parent(var, slot))

    # Bluedroid Classic BR/EDR-only + HID Host; BLE fully disabled on this node
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_CLASSIC_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLE_ENABLED", False)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY", True)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BTDM", False)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BLE_ONLY", False)
    add_idf_sdkconfig_option("CONFIG_BT_HID_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_HID_HOST_ENABLED", True)
    # two simultaneous Classic ACL links (one per hosted remote)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_BR_EDR_MAX_ACL_CONN", 2)
