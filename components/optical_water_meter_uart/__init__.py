import esphome.codegen as cg
from esphome.components import uart

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor"]
ns = cg.esphome_ns.namespace("optical_water_meter_uart")
OpticalWaterMeterUART = ns.class_("OpticalWaterMeterUART", cg.PollingComponent, uart.UARTDevice)
