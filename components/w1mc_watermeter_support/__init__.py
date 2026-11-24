"""W1MC watermeter helpers, distributed with the external component source."""
import esphome.codegen as cg
import esphome.config_validation as cv

DEPENDENCIES = ["esp32", "wifi"]
CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    cg.add_global(cg.RawStatement(
        '#include "esphome/components/w1mc_watermeter_support/w1mc_watermeter_support.h"'
    ))
