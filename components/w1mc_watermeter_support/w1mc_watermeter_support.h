#pragma once

#include "esphome/core/preferences.h"
#include "esphome/core/log.h"
#include "nvs.h"

// ESPHome 2026.9.1 WiFiComponent::start() uses preference key 88491487
// when no STA credentials are compiled into the YAML. Delete only that key:
// global_preferences->reset() would also erase watermeter state.
// Keep the W1MC YAML provisioned via Improv/captive portal, without wifi.ssid
// or wifi.networks. Recheck this key when changing ESPHome's WiFi persistence.
inline bool w1mc_clear_provisioned_wifi() {
  if (!esphome::global_preferences->sync()) {
    ESP_LOGE("w1mc.button", "Pending preferences could not be saved");
    return false;
  }
  nvs_handle_t handle;
  esp_err_t err = nvs_open("esphome", NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGE("w1mc.button", "WiFi storage could not be opened: %d", err);
    return false;
  }
  err = nvs_erase_key(handle, "88491487");
  if (err == ESP_ERR_NVS_NOT_FOUND)
    err = ESP_OK;
  if (err == ESP_OK)
    err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) {
    ESP_LOGE("w1mc.button", "WiFi credentials could not be cleared: %d", err);
    return false;
  }
  ESP_LOGI("w1mc.button", "WiFi credentials cleared; meter state and calibration preserved");
  return true;
}
