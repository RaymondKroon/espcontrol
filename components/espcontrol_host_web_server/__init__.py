import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT

CODEOWNERS = ["@jtenniswood"]
DEPENDENCIES = ["host"]

CONF_JS_PATH = "js_path"

ns = cg.esphome_ns.namespace("espcontrol_host_web_server")
HostWebServer = ns.class_("HostWebServer", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HostWebServer),
        cv.Optional(CONF_PORT, default=8080): cv.port,
        cv.Required(CONF_JS_PATH): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_js_path(config[CONF_JS_PATH]))
