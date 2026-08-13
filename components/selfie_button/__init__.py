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
