"""Display platform for the ELECROW CrowPanel ESP32-S3 5.79" e-paper.

The board's wiring is fixed, so every pin defaults to it and the YAML only says
how to draw:

    spi:
      clk_pin: GPIO12
      mosi_pin: GPIO11
    display:
      - platform: crowpanel_579
        lambda: ...
"""

from esphome import pins
import esphome.codegen as cg
from esphome.components import display, sensor, spi
from esphome.components.crowpanel_epaper import CrowPanelBase
import esphome.config_validation as cv
from esphome.const import (
    CONF_BUSY_PIN,
    CONF_CS_PIN,
    CONF_DC_PIN,
    CONF_FULL_UPDATE_EVERY,
    CONF_ID,
    CONF_LAMBDA,
    CONF_PAGES,
    CONF_RESET_PIN,
    CONF_UPDATE_INTERVAL,
)

# epaper_spi supplies the base class; its own platform is not used.
AUTO_LOAD = ["crowpanel_epaper", "epaper_spi", "split_buffer"]
DEPENDENCIES = ["spi"]

CONF_GREY = "grey"
CONF_GREY_LEVELS = "grey_levels"
CONF_GREY_PARTIALS = "grey_partials"
CONF_POWER_PIN = "power_pin"
CONF_TEMPERATURE_SENSOR = "temperature_sensor"

crowpanel_579_ns = cg.esphome_ns.namespace("crowpanel_579")
CrowPanel579 = crowpanel_579_ns.class_("CrowPanel579", CrowPanelBase)


def _seven_needs_grey(config):
    if config[CONF_GREY_LEVELS] == 7 and not config[CONF_GREY]:
        raise cv.Invalid("grey_levels: 7 needs grey: true")
    if config[CONF_GREY_PARTIALS] and not config[CONF_GREY]:
        raise cv.Invalid("grey_partials needs grey: true")
    return config


CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        spi.spi_device_schema(
            cs_pin_required=False, default_mode="MODE0", default_data_rate=20e6
        )
    ).extend(
        {
            cv.GenerateID(): cv.declare_id(CrowPanel579),
            cv.Optional(CONF_CS_PIN, default="GPIO45"): pins.gpio_output_pin_schema,
            cv.Optional(CONF_DC_PIN, default="GPIO46"): pins.gpio_output_pin_schema,
            cv.Optional(CONF_RESET_PIN, default="GPIO47"): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_BUSY_PIN, default="GPIO48"): pins.gpio_input_pin_schema,
            # Switches the panel's ground through Q9; nothing answers until it is high.
            cv.Optional(CONF_POWER_PIN, default="GPIO7"): pins.internal_gpio_output_pin_schema,
            # Partial refreshes between full ones, which clear the ghosting.
            cv.Optional(CONF_FULL_UPDATE_EVERY, default=30): cv.int_range(1, 255),
            # Full refreshes in four greys (about 3.9 s) instead of the panel's own
            # black and white (about 3.7 s). Partial refreshes stay black and white.
            cv.Optional(CONF_GREY, default=False): cv.boolean,
            # 7 runs each grey refresh in three passes, with a half step between each of the
            # four greys (about 4.4 s). Needs grey.
            cv.Optional(CONF_GREY_LEVELS, default=4): cv.one_of(4, 7, int=True),
            # "glide" walks each changed pixel to its new grey on a partial update, without
            # flashing, rather than pushing it to black or white; the next full refresh settles the
            # greys exactly. "live" makes each update a short tick towards the frame drawn, ticking
            # on until everything arrives, for things that move. Needs grey, and PSRAM.
            cv.Optional(CONF_GREY_PARTIALS, default=False): cv.Any(
                cv.boolean, cv.one_of("glide", "live", lower=True)
            ),
            # The panel's temperature, or near enough: the waveforms run faster in the warm and
            # slower in the cold, in the panel maker's bands. Without one they run as at 13 to 20 °C.
            cv.Optional(CONF_TEMPERATURE_SENSOR): cv.use_id(sensor.Sensor),
            # Something else decides when to draw (an interval, HA, the dial).
            cv.Optional(CONF_UPDATE_INTERVAL, default="never"): cv.update_interval,
        }
    ),
    cv.has_at_most_one_key(CONF_PAGES, CONF_LAMBDA),
    _seven_needs_grey,
)

FINAL_VALIDATE_SCHEMA = spi.final_validate_device_schema(
    "crowpanel_579", require_miso=False, require_mosi=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)
    await spi.register_spi_device(var, config, write_only=True)

    cg.add(var.set_dc_pin(await cg.gpio_pin_expression(config[CONF_DC_PIN])))
    reset = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
    cg.add(var.set_reset_pin(reset))
    cg.add(var.set_hold_reset_pin(reset))
    cg.add(var.set_busy_pin(await cg.gpio_pin_expression(config[CONF_BUSY_PIN])))
    power = await cg.gpio_pin_expression(config[CONF_POWER_PIN])
    cg.add(var.set_enable_pins([power]))
    cg.add(var.set_power_pin(power))
    cg.add(var.set_full_update_every(config[CONF_FULL_UPDATE_EVERY]))
    cg.add(var.set_grey(config[CONF_GREY]))
    cg.add(var.set_grey_levels(config[CONF_GREY_LEVELS]))
    if config[CONF_GREY_PARTIALS] == "glide":
        cg.add(var.set_glide(True))
    elif config[CONF_GREY_PARTIALS] == "live":
        cg.add(var.set_live(True))
    elif config[CONF_GREY_PARTIALS] is True:
        raise cv.Invalid("the 5.79\" has no partial window: grey_partials takes glide or live")
    if temperature := config.get(CONF_TEMPERATURE_SENSOR):
        cg.add(var.set_temperature_sensor(await cg.get_variable(temperature)))

    if lambda_config := config.get(CONF_LAMBDA):
        lambda_ = await cg.process_lambda(
            lambda_config, [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))
