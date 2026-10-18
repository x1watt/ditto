// Ditto firmware entry point.
// Milestone 1+3: USB composite HID (keyboard+mouse) + LAN HTTP control API.
// MSC, HDMI capture and LVGL UI follow in later milestones.
#include "esp_log.h"

#include "app_config.h"
#include "usb_descriptors.h"
#include "hid_engine.h"
#include "storage.h"
#include "ui.h"
#include "capture.h"
#include "net_api.h"

static const char *TAG = "ditto";

void app_main(void)
{
    ESP_LOGI(TAG, "Ditto starting");

    ESP_ERROR_CHECK(app_config_init());
    ESP_LOGW(TAG, "API token: %s", app_config_token());

    hid_engine_init();
    ESP_ERROR_CHECK(storage_init());  // MSC backing store; must precede USB install
    usb_device_init();   // enumerate as keyboard + mouse + disk to the target
    ui_init();           // local TFT status screen (no-op if disabled)
    // HDMI capture is best-effort: a wiring/receiver fault must not stop HID+API.
    esp_err_t cap = capture_init();
    if (cap != ESP_OK) ESP_LOGW(TAG, "capture init failed: %s", esp_err_to_name(cap));
    net_api_start();     // join LAN, serve the control API

    ESP_LOGI(TAG, "Ditto ready");
}
