#pragma once
#include "esp_err.h"

// Load config from NVS. Generates and persists a random bearer token on first
// boot. Falls back to menuconfig Wi-Fi defaults when NVS has none.
esp_err_t app_config_init(void);

// 32-char hex bearer token required on every API request. Never NULL after init.
const char *app_config_token(void);

const char *app_config_wifi_ssid(void);
const char *app_config_wifi_password(void);
const char *app_config_hostname(void);
