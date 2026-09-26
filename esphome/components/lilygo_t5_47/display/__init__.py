from esphome import pins
import esphome.codegen as cg
from esphome.components import display, esp32
import esphome.config_validation as cv
from esphome.const import (
    CONF_DATA_PINS,
    CONF_ID,
    CONF_IGNORE_STRAPPING_WARNING,
    CONF_LAMBDA,
    CONF_NUMBER,
)

from .. import lilygo_t5_47_ns

DEPENDENCIES = ["esp32", "psram"]

CONF_CFG_CLOCK_PIN = "cfg_clock_pin"
CONF_CFG_DATA_PIN = "cfg_data_pin"
CONF_CFG_STROBE_PIN = "cfg_strobe_pin"
CONF_CKH_PIN = "ckh_pin"
CONF_CKV_PIN = "ckv_pin"
CONF_STH_PIN = "sth_pin"

LilygoT547Display = lilygo_t5_47_ns.class_("LilygoT547Display", display.DisplayBuffer)


def _strapping_pin(number):
    return {CONF_NUMBER: number, CONF_IGNORE_STRAPPING_WARNING: True}


# Panel data lines D0..D7 of the ESP32 (WROVER-E) version of the board.
DEFAULT_DATA_PINS = [33, 32, 4, 19, _strapping_pin(2), 27, 21, 22]

CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(LilygoT547Display),
            cv.Optional(CONF_DATA_PINS, default=DEFAULT_DATA_PINS): cv.All(
                [pins.internal_gpio_output_pin_schema],
                cv.Length(min=8, max=8),
            ),
            cv.Optional(
                CONF_CFG_DATA_PIN, default=23
            ): pins.internal_gpio_output_pin_schema,
            cv.Optional(
                CONF_CFG_CLOCK_PIN, default=18
            ): pins.internal_gpio_output_pin_schema,
            cv.Optional(
                CONF_CFG_STROBE_PIN, default=_strapping_pin(0)
            ): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_CKV_PIN, default=25): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_STH_PIN, default=26): pins.internal_gpio_output_pin_schema,
            cv.Optional(
                CONF_CKH_PIN, default=_strapping_pin(5)
            ): pins.internal_gpio_output_pin_schema,
        }
    ).extend(cv.polling_component_schema("60s")),
    esp32.only_on_variant(supported=[esp32.VARIANT_ESP32]),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)

    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))

    for index, pin_config in enumerate(config[CONF_DATA_PINS]):
        pin = await cg.gpio_pin_expression(pin_config)
        cg.add(var.set_data_pin(index, pin))

    for key, setter in (
        (CONF_CFG_DATA_PIN, var.set_cfg_data_pin),
        (CONF_CFG_CLOCK_PIN, var.set_cfg_clock_pin),
        (CONF_CFG_STROBE_PIN, var.set_cfg_strobe_pin),
        (CONF_CKV_PIN, var.set_ckv_pin),
        (CONF_STH_PIN, var.set_sth_pin),
        (CONF_CKH_PIN, var.set_ckh_pin),
    ):
        pin = await cg.gpio_pin_expression(config[key])
        cg.add(setter(pin))
