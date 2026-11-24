#pragma once
#include "receiver.h"
#include "binary_transport.h"
#include "flow_rate.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

namespace esphome {
namespace optical_water_meter_uart {
enum Numeric { METER_READING, TEST_LITERS, TEST_STEPS, LOCAL_STEPS, PHASE, REJECTED, INVALID,
  DARK_RAIL, BOOT, PARSE_ERRORS, RAW_GAPS, ADC_ERRORS, UART_ERRORS, FORMAT_ERRORS, STORE_ERRORS,
  RAW_START, RANGE_START=24, CRC_ERRORS=30, FRAMING_ERRORS, SEQUENCE_ERRORS, FRAME_GAPS,
  PROTOCOL_ERRORS, UNKNOWN_FRAMES, RECEIVED_FRAMES, FLOW_RATE, NUMERIC_COUNT };
enum Text { CALIBRATION_STATUS, SENSOR_IDENTITY, LAST_RAW, LAST_METER, LAST_INFO, LAST_STATUS, TEXT_COUNT };
enum Binary { MEASUREMENT_VALID, INTERRUPTED, STORAGE_PROBLEM, SENSOR_CONNECTED, CALIBRATION_RESTORED, BINARY_COUNT };

class OpticalWaterMeterUART : public PollingComponent, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  void on_shutdown() override { save_(true); }
  float get_setup_priority() const override { return setup_priority::DATA; }
  void set_numeric(unsigned i,sensor::Sensor *s) { if (i<NUMERIC_COUNT) numeric_[i]=s; }
  void set_text(unsigned i,text_sensor::TextSensor *s) { if (i<TEXT_COUNT) texts_[i]=s; }
  void set_binary(unsigned i,binary_sensor::BinarySensor *s) { if (i<BINARY_COUNT) binaries_[i]=s; }
  void set_storage_key(uint32_t k) { storage_key_=k; }
  void set_log_raw_frames(bool v) { log_raw_=v; }
  void set_binary_protocol(bool v) { binary_protocol_=v; }
  double get_meter_m3() const { return receiver_.meter_m3(); }
  void set_meter_m3(double value);
  void reset_test();
 protected:
  optical_uart::Receiver receiver_;
  optical_uart::FlowRate flow_rate_;
  double current_flow_rate_ = NAN;
  optical_uart::BinaryTransport binary_{receiver_};
  ESPPreferenceObject preference_;
  optical_uart::Checkpoint last_saved_{};
  bool have_saved_=false, storage_problem_=false, dirty_=false, log_raw_=false, binary_protocol_=false;
  uint32_t storage_key_=0x55415231, last_save_=0;
  optical_uart::LineBuffer framer_;
  char raw_line_[512]{}, meter_line_[512]{}, info_line_[512]{}, status_line_[512]{};
  sensor::Sensor *numeric_[NUMERIC_COUNT]{};
  text_sensor::TextSensor *texts_[TEXT_COUNT]{};
  binary_sensor::BinarySensor *binaries_[BINARY_COUNT]{};
  void save_(bool force=false);
  void publish_numeric_(unsigned i,double v) { if (numeric_[i]) numeric_[i]->publish_state(v); }
  void publish_text_(unsigned i,const char *v) { if (texts_[i]) texts_[i]->publish_state(v); }
};
}  // namespace optical_water_meter_uart
}  // namespace esphome
