#include "optical_water_meter_uart.h"
#include "esphome/core/log.h"
#include <cstdio>

namespace esphome {
namespace optical_water_meter_uart {
static const char *const TAG="optical_water_meter_uart";

void OpticalWaterMeterUART::setup() {
  preference_=global_preferences->make_preference<optical_uart::Checkpoint>(storage_key_,true);
  optical_uart::Checkpoint p{};
  if (preference_.load(&p) && receiver_.restore(p)) {
    last_saved_=p; have_saved_=true;
    ESP_LOGI(TAG,"UART meter checkpoint restored; verify physical meter reading");
  }
  last_save_=millis();
  update();
}
void OpticalWaterMeterUART::dump_config() {
  ESP_LOGCONFIG(TAG,"STM32 local counter PRIMARY / %s / 6 steps per liter",binary_protocol_?"BINARY1 1.0.0":"TEXT2 0.5.0");
  ESP_LOGCONFIG(TAG,"RAW diagnostic only; count confirmation at STATUS (~5s)");
  ESP_LOGCONFIG(TAG,"Transport CRC: %s; checkpoint: 60s",binary_protocol_?"CRC-16/CCITT-FALSE":"none (legacy)");
}
void OpticalWaterMeterUART::loop() {
  uint32_t now=millis();
  if (binary_protocol_) binary_.expire(now);
  else if (framer_.expire(now)) receiver_.malformed();
  // Bound work per loop so diagnostics cannot starve the network/watchdog.
  for (unsigned budget=0; budget<4096 && available(); ++budget) {
    uint8_t b;
    if (!read_byte(&b)) break;
    if (binary_protocol_) {
      uint8_t type=binary_.push(b,now);
      if (type==OW_RAW) {
        std::strcpy(raw_line_,binary_.raw_text);
        if (log_raw_) { ESP_LOGD(TAG,"%s",raw_line_); }
      } else if (type==OW_METER) {
        std::strcpy(meter_line_,binary_.meter_text);
        ESP_LOGD(TAG,"%s",meter_line_);
      } else if (type==OW_INFO) {
        std::strcpy(info_line_,binary_.info_text);
        ESP_LOGD(TAG,"%s",info_line_);
      } else if (type==OW_STATUS) {
        std::strcpy(status_line_,binary_.status_text);
        ESP_LOGD(TAG,"%s",status_line_);
      }
      if (type && receiver_.committed) dirty_=true;
      continue;
    }
    int event=framer_.push(b,now);
    if (event<0) receiver_.malformed();
    if (event==1) {
        const char *line=framer_.data;
        bool ok=receiver_.ingest(line,now);
        if (ok) {
          if (!std::strncmp(line,"RAW,",4)) {
            std::strcpy(raw_line_,line);
            if (log_raw_) { ESP_LOGD(TAG,"%s",line); }
          } else if (!std::strncmp(line,"METER,",6)) {
            std::strcpy(meter_line_,line);
            ESP_LOGD(TAG,"%s",line);
          } else if (!std::strncmp(line,"INFO,",5)) {
            std::strcpy(info_line_,line);
          } else if (!std::strncmp(line,"STATUS,",7)) {
            std::strcpy(status_line_,line);
          }
          if (receiver_.committed) dirty_=true;
        }
    }
  }
  receiver_.tick(now);
  if (numeric_[FLOW_RATE]) {
    current_flow_rate_=flow_rate_.update(now,receiver_.valid(now),receiver_.saved.total_steps,
      receiver_.saved.time_ms,receiver_.saved.boot,receiver_.saved.device);
  }
  save_();
}
void OpticalWaterMeterUART::save_(bool force) {
  if (!dirty_ || (!force && millis()-last_save_<60000)) return;
  last_save_=millis();
  if (have_saved_ && !std::memcmp(&last_saved_,&receiver_.saved,sizeof(last_saved_))) { dirty_=false; return; }
  bool ok=preference_.save(&receiver_.saved) && global_preferences->sync();
  storage_problem_=!ok;
  if (ok) { last_saved_=receiver_.saved; have_saved_=true; dirty_=false; }
  else ESP_LOGW(TAG,"Could not save meter checkpoint");
}
void OpticalWaterMeterUART::set_meter_m3(double value) {
  if (!receiver_.set_meter(value,millis())) {
    ESP_LOGW(TAG,"Meter reading requires a valid identified READY sensor and a finite value 0..99999");
    return;
  }
  dirty_=true; save_(true); update();
}
void OpticalWaterMeterUART::reset_test() {
  receiver_.reset_trial(); dirty_=true; save_(true); update();
}
void OpticalWaterMeterUART::update() {
  uint32_t now=millis(); receiver_.tick(now);
  auto &r=receiver_; auto &s=r.status;
  publish_numeric_(METER_READING,r.meter_m3());
  publish_numeric_(FLOW_RATE,r.valid(now)?current_flow_rate_:NAN);
  publish_numeric_(TEST_LITERS,r.trial_liters());
  publish_numeric_(TEST_STEPS,static_cast<double>(r.saved.total_steps)-r.saved.trial_start);
  publish_numeric_(LOCAL_STEPS,static_cast<double>(r.meter.steps));
  publish_numeric_(PHASE,r.meter.phase);
  publish_numeric_(REJECTED,r.meter.rejected); publish_numeric_(INVALID,r.meter.invalid);
  publish_numeric_(DARK_RAIL,r.meter.dark_rail); publish_numeric_(BOOT,s.boot);
  publish_numeric_(PARSE_ERRORS,r.parse_errors); publish_numeric_(RAW_GAPS,r.raw_gaps);
  publish_numeric_(ADC_ERRORS,s.adc_errors); publish_numeric_(UART_ERRORS,s.uart_errors);
  publish_numeric_(FORMAT_ERRORS,s.format_errors); publish_numeric_(STORE_ERRORS,s.store_errors);
  publish_numeric_(CRC_ERRORS,binary_.crc_errors); publish_numeric_(FRAMING_ERRORS,binary_.framing_errors);
  publish_numeric_(SEQUENCE_ERRORS,binary_.sequence_errors); publish_numeric_(FRAME_GAPS,binary_.frame_gaps);
  publish_numeric_(PROTOCOL_ERRORS,binary_.protocol_errors); publish_numeric_(UNKNOWN_FRAMES,binary_.unknown_frames);
  publish_numeric_(RECEIVED_FRAMES,binary_.received_frames);
  for (unsigned i=0;i<3;++i) {
    publish_numeric_(RAW_START+i*3,r.raw.off[i]);
    publish_numeric_(RAW_START+i*3+1,r.raw.on[i]);
    publish_numeric_(RAW_START+i*3+2,static_cast<int>(r.raw.off[i])-r.raw.on[i]);
    publish_numeric_(RANGE_START+i*2,s.low[i]); publish_numeric_(RANGE_START+i*2+1,s.high[i]);
  }
  const char *state=!r.profile_ok ? (binary_protocol_?"Wachten op passende INFO (BINARY1 / 1.0.0)":"Wachten op passende INFO (TEXT2 / 0.5.0)") :
    (!r.online(now) ? "Wachten op actuele RAW / METER / STATUS" : r.meter.state);
  publish_text_(CALIBRATION_STATUS,state);
  char identity[64];
  std::snprintf(identity,sizeof(identity),"%s / boot %lu",r.saved.device,static_cast<unsigned long>(s.boot));
  publish_text_(SENSOR_IDENTITY,identity);
  publish_text_(LAST_RAW,raw_line_); publish_text_(LAST_METER,meter_line_);
  publish_text_(LAST_INFO,info_line_); publish_text_(LAST_STATUS,status_line_);
  bool states[BINARY_COUNT]={r.valid(now),r.interrupted,storage_problem_,r.online(now),s.restored};
  for (unsigned i=0;i<BINARY_COUNT;++i) if (binaries_[i]) binaries_[i]->publish_state(states[i]);
}
}  // namespace optical_water_meter_uart
}  // namespace esphome
