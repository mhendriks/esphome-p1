import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, sensor, text_sensor, uart
from esphome.const import CONF_ID, ENTITY_CATEGORY_DIAGNOSTIC, STATE_CLASS_MEASUREMENT, STATE_CLASS_TOTAL, DEVICE_CLASS_WATER
from . import OpticalWaterMeterUART

# Keep indices in sync with Numeric/Text/Binary in optical_water_meter_uart.h.
NUMERIC = ["meter_reading", "test_liters", "test_steps", "local_steps", "phase",
           "rejected_transitions", "invalid_pairs", "dark_rail", "boot", "parse_errors", "raw_gaps",
           "adc_errors", "uart_errors", "format_errors", "store_errors"]
NUMERIC += [f"raw_{p}_{kind}" for p in "abc" for kind in ("dark", "light", "difference")]
NUMERIC += [f"learned_{p}_{bound}" for p in "abc" for bound in ("low", "high")]
NUMERIC += ["crc_errors", "framing_errors", "sequence_errors", "frame_gaps", "protocol_errors", "unknown_frames", "received_frames"]
NUMERIC += ["flow_rate"]
TEXT = ["calibration_status", "sensor_identity", "last_raw", "last_meter", "last_info", "last_status"]
BINARY = ["measurement_valid", "interrupted", "storage_problem", "sensor_connected", "calibration_restored"]

diagnostic = sensor.sensor_schema(accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
                                  entity_category=ENTITY_CATEGORY_DIAGNOSTIC)
schemas = {key: diagnostic for key in NUMERIC}
schemas["meter_reading"] = sensor.sensor_schema(unit_of_measurement="m³", accuracy_decimals=3,
    device_class=DEVICE_CLASS_WATER, state_class=STATE_CLASS_TOTAL)
schemas["test_liters"] = sensor.sensor_schema(unit_of_measurement="L", accuracy_decimals=3,
    device_class=DEVICE_CLASS_WATER, state_class=STATE_CLASS_MEASUREMENT)
schemas["flow_rate"] = sensor.sensor_schema(unit_of_measurement="L/min", accuracy_decimals=2,
    device_class="volume_flow_rate", state_class=STATE_CLASS_MEASUREMENT)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(OpticalWaterMeterUART),
    cv.Optional("storage_id", default=0x55415231): cv.hex_uint32_t,
    cv.Optional("log_raw_frames", default=False): cv.boolean,
    cv.Optional("protocol", default="TEXT2"): cv.one_of("TEXT2", "BINARY1", upper=True),
    **{cv.Optional(key): schema for key, schema in schemas.items()},
    **{cv.Optional(key): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC) for key in TEXT},
    **{cv.Optional(key): binary_sensor.binary_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC) for key in BINARY},
}).extend(cv.polling_component_schema("1s")).extend(uart.UART_DEVICE_SCHEMA)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "optical_water_meter_uart", baud_rate=115200, data_bits=8, parity="NONE", stop_bits=1, require_rx=True)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_storage_key(config["storage_id"]))
    cg.add(var.set_log_raw_frames(config["log_raw_frames"]))
    cg.add(var.set_binary_protocol(config["protocol"] == "BINARY1"))
    for index, key in enumerate(NUMERIC):
        if key in config:
            entity = await sensor.new_sensor(config[key])
            cg.add(var.set_numeric(index, entity))
    for index, key in enumerate(TEXT):
        if key in config:
            entity = await text_sensor.new_text_sensor(config[key])
            cg.add(var.set_text(index, entity))
    for index, key in enumerate(BINARY):
        if key in config:
            entity = await binary_sensor.new_binary_sensor(config[key])
            cg.add(var.set_binary(index, entity))
