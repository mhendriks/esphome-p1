#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace optical_uart {
// Average two confirmed STATUS intervals (~10s), using STM32 sample time.
// Never differentiate the adjustable meter offset or the resettable trial.
class FlowRate {
 public:
  double update(uint32_t now, bool valid, int64_t steps, uint32_t time_ms,
                uint32_t boot, const char *device) {
    if (!valid) { reset(); return NAN; }
    if (count_ && (boot != boot_ || std::strcmp(device, device_))) reset();
    if (count_ && now - last_received_ >= 7000) reset();
    if (count_ && time_ms == samples_[count_ - 1].time_ms) return value_;
    if (count_ && time_ms - samples_[count_ - 1].time_ms >= 0x80000000U) reset();
    if (!count_) {
      boot_ = boot;
      std::strncpy(device_, device, sizeof(device_) - 1);
    }
    if (count_ == 3) {
      samples_[0] = samples_[1]; samples_[1] = samples_[2]; count_ = 2;
    }
    samples_[count_++] = {steps, time_ms};
    last_received_ = now;
    if (count_ == 3) {
      const uint32_t elapsed = time_ms - samples_[0].time_ms;
      const double delta = static_cast<double>(steps) - samples_[0].steps;
      // Six net phase steps per liter. Reverse motion is not consumption.
      value_ = elapsed ? std::fmax(0.0, delta) * 10000.0 / elapsed : NAN;
    }
    return value_;
  }
  void reset() { count_ = 0; value_ = NAN; }
 private:
  struct Sample { int64_t steps; uint32_t time_ms; } samples_[3]{};
  unsigned count_ = 0;
  uint32_t boot_ = 0, last_received_ = 0;
  char device_[25]{};
  double value_ = NAN;
};
}  // namespace optical_uart
