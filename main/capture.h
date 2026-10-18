#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Start the capture pipeline (I2C receiver config + DVP + JPEG task). No-op and
// returns ESP_OK if capture is disabled in menuconfig.
esp_err_t capture_init(void);

// True once at least one frame has been encoded.
bool capture_available(void);

// Copy the latest JPEG into a freshly heap_caps-allocated buffer (caller frees
// with free()). *len is the byte count. Returns ESP_ERR_NOT_FOUND if no frame
// yet, ESP_ERR_INVALID_STATE if capture is disabled.
esp_err_t capture_get_jpeg(uint8_t **out, size_t *len);
