import esphome.codegen as cg
from esphome.components import climate_ir
import esphome.config_validation as cv
from esphome.const import CONF_MODEL
from esphome.types import ConfigType

from . import gree_ns

CODEOWNERS = ["@orestismers", "@Rian23-R4"]

AUTO_LOAD = ["climate_ir"]
GreeClimate = gree_ns.class_("GreeClimate", climate_ir.ClimateIR)

Model = gree_ns.enum("Model")
MODELS = {
    "generic": Model.GREE_GENERIC,
    "yan": Model.GREE_YAN,
    "yaa": Model.GREE_YAA,
    "yac": Model.GREE_YAC,
    "yac1fb9": Model.GREE_YAC1FB9,
    "yx1ff": Model.GREE_YX1FF,
    "yag": Model.GREE_YAG,
}

# DRED (demand-response power limitation) + Eco (SE) presets.
# Adds presets to the climate entity:
#   Eco   -> SE "Save Energy" (official Gree Econo bit)
#   Away  -> DRED d2 (~50% power cap)
#   Boost -> DRED d3 (~75% power cap)
# Eco and DRED are mutually exclusive on these units; the sequences handle that.
CONF_DRED = "dred"

CONFIG_SCHEMA = climate_ir.climate_ir_with_receiver_schema(GreeClimate).extend(
    {
        cv.Required(CONF_MODEL): cv.enum(MODELS, lower=True),
        cv.Optional(CONF_DRED, default=False): cv.boolean,
    }
)


async def to_code(config: ConfigType) -> None:
    var = await climate_ir.new_climate_ir(config)
    cg.add(var.set_model(config[CONF_MODEL]))
    cg.add(var.set_dred_support(config[CONF_DRED]))
