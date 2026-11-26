"""What ELECROW's CrowPanel e-paper drivers share. The boards' display platforms load it."""

import esphome.codegen as cg
from esphome.components.epaper_spi.display import EPaperBase

AUTO_LOAD = ["epaper_spi", "split_buffer"]

CONFIG_SCHEMA = {}

crowpanel_epaper_ns = cg.esphome_ns.namespace("crowpanel_epaper")
CrowPanelBase = crowpanel_epaper_ns.class_("CrowPanelBase", EPaperBase)
