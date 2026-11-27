"""Turbo display platform for the ELECROW CrowPanel ESP32-S3 4.2", V1.2: `crowpanel_42` without
charge balance. A full refresh is one push of every pixel to black or white, as far as it has to
go, then the greys dosed from white, five to eight times faster; nothing is recorded, cancelled or
settled. Every refresh leaves its drive on the panel, which shows as image retention and may
shorten the panel's life: for experiments, not for a display left running. Takes the same options
as `crowpanel_42`, and needs `grey: true`, since each refresh starts from the levels last drawn.
"""

import esphome.codegen as cg
from esphome.components import spi
from esphome.components.crowpanel_42 import display as safe
import esphome.config_validation as cv
from esphome.const import CONF_ID

AUTO_LOAD = ["crowpanel_42", "crowpanel_epaper", "epaper_spi", "split_buffer"]
DEPENDENCIES = ["spi"]


def _needs_grey(config):
    if not config[safe.CONF_GREY]:
        raise cv.Invalid("turbo needs grey: true")
    return config


CONFIG_SCHEMA = cv.All(safe.CONFIG_SCHEMA, _needs_grey)

FINAL_VALIDATE_SCHEMA = spi.final_validate_device_schema(
    "crowpanel_42_turbo", require_miso=False, require_mosi=True
)


async def to_code(config):
    await safe.to_code(config)
    var = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_balanced(False))
