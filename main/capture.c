// HDMI capture: an HDMI receiver (TC358743) feeds its parallel video output into
// the S3 LCD_CAM (DVP) controller as RGB565 frames; a task software-JPEG-encodes
// the latest frame for /screenshot. The S3 has no hardware JPEG, so this is
// deliberately low-res / low-fps. Per performance.md: bounded buffers, encode once
// per captured frame (shared by all readers), nothing here runs on the LVGL task.
#include "capture.h"
#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "capture";

#if !CONFIG_DITTO_CAPTURE_ENABLE

esp_err_t capture_init(void) { ESP_LOGI(TAG, "capture disabled"); return ESP_OK; }
bool capture_available(void) { return false; }
esp_err_t capture_get_jpeg(uint8_t **out, size_t *len) { (void)out; (void)len; return ESP_ERR_INVALID_STATE; }

#else

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"

#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_jpeg_enc.h"

#define CAM_W        CONFIG_DITTO_CAM_HRES
#define CAM_H        CONFIG_DITTO_CAM_VRES
#define FRAME_BYTES  ((size_t)CAM_W * CAM_H * 2)   // RGB565
#define JPEG_CAP     ((size_t)CAM_W * CAM_H)        // generous upper bound at q<=60

static esp_cam_ctlr_handle_t s_cam;
static jpeg_enc_handle_t      s_jpeg;
static uint8_t *s_frame[2];        // double-buffered DVP target (PSRAM)
static uint8_t *s_jpeg_buf;        // latest encoded JPEG (PSRAM)
static size_t   s_jpeg_len;
static int64_t  s_frame_us;
static SemaphoreHandle_t s_lock;

// ---------------------------------------------------------------------------
// HDMI receiver (TC358743) I2C configuration.
//
// BOARD-SPECIFIC AND UNVERIFIED. The TC358743 needs a sizable register sequence
// to lock to the HDMI input timing and emit its parallel output in the chosen
// pixel format; the exact values depend on the input resolution and the breakout
// wiring. This stub only proves the I2C link. Fill the register writes for your
// board/input before expecting frames. See the TC358743 datasheet + Linux
// tc358743 driver as references.
// ---------------------------------------------------------------------------
static esp_err_t tc358743_configure(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_DITTO_CAM_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_DITTO_CAM_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) return err;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_DITTO_CAM_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) return err;

    // Probe the 16-bit ChipID register (0x0000). Just confirms the link is alive.
    uint8_t reg[2] = {0x00, 0x00};
    uint8_t id[2] = {0};
    err = i2c_master_transmit_receive(dev, reg, 2, id, 2, 100);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "receiver ChipID reg = 0x%02x%02x (configure regs for your input!)", id[0], id[1]);
    else
        ESP_LOGW(TAG, "receiver I2C probe failed: %s", esp_err_to_name(err));

    // TODO(board): write the HDMI-lock + parallel-output register sequence here.
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// DVP controller
// ---------------------------------------------------------------------------
static esp_err_t dvp_start(void)
{
    const esp_cam_ctlr_dvp_pin_config_t pins = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .data_io = {
            CONFIG_DITTO_CAM_D0_GPIO, CONFIG_DITTO_CAM_D1_GPIO,
            CONFIG_DITTO_CAM_D2_GPIO, CONFIG_DITTO_CAM_D3_GPIO,
            CONFIG_DITTO_CAM_D4_GPIO, CONFIG_DITTO_CAM_D5_GPIO,
            CONFIG_DITTO_CAM_D6_GPIO, CONFIG_DITTO_CAM_D7_GPIO,
        },
        .vsync_io = CONFIG_DITTO_CAM_VSYNC_GPIO,
        .de_io    = CONFIG_DITTO_CAM_DE_GPIO,
        .pclk_io  = CONFIG_DITTO_CAM_PCLK_GPIO,
        .xclk_io  = CONFIG_DITTO_CAM_XCLK_GPIO,
    };
    esp_cam_ctlr_dvp_config_t cfg = {
        .ctlr_id = 0,
        .clk_src = CAM_CLK_SRC_DEFAULT,
        .h_res = CAM_W,
        .v_res = CAM_H,
        .input_data_color_type = CAM_CTLR_COLOR_RGB565,
        .dma_burst_size = 128,
        .pin = &pins,
    };
    ESP_RETURN_ON_ERROR(esp_cam_new_dvp_ctlr(&cfg, &s_cam), TAG, "dvp new");
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_enable(s_cam), TAG, "dvp enable");
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_start(s_cam), TAG, "dvp start");
    return ESP_OK;
}

static void capture_task(void *arg)
{
    (void)arg;
    int idx = 0;
    while (1) {
        esp_cam_ctlr_trans_t trans = { .buffer = s_frame[idx], .buflen = FRAME_BYTES };
        if (esp_cam_ctlr_receive(s_cam, &trans, ESP_CAM_CTLR_MAX_DELAY) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int out_len = 0;
        jpeg_error_t jerr = jpeg_enc_process(s_jpeg, trans.buffer, FRAME_BYTES,
                                             s_jpeg_buf + 0, JPEG_CAP, &out_len);
        if (jerr == JPEG_ERR_OK && out_len > 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_jpeg_len = out_len;
            s_frame_us = esp_timer_get_time();
            xSemaphoreGive(s_lock);
        }
        idx ^= 1;
    }
}

esp_err_t capture_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < 2; ++i) {
        s_frame[i] = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_frame[i]) return ESP_ERR_NO_MEM;
    }
    s_jpeg_buf = heap_caps_malloc(JPEG_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_jpeg_buf) return ESP_ERR_NO_MEM;

    jpeg_enc_config_t jcfg = {
        .width = CAM_W,
        .height = CAM_H,
        .src_type = JPEG_PIXEL_FORMAT_RGB565_LE,
        .subsampling = JPEG_SUBSAMPLE_420,
        .quality = CONFIG_DITTO_CAM_JPEG_QUALITY,
        .rotate = JPEG_ROTATE_0D,
        .task_enable = false,
    };
    if (jpeg_enc_open(&jcfg, &s_jpeg) != JPEG_ERR_OK) return ESP_FAIL;

    ESP_RETURN_ON_ERROR(tc358743_configure(), TAG, "receiver cfg");
    ESP_RETURN_ON_ERROR(dvp_start(), TAG, "dvp");

    // Encode is CPU-heavy: pin to core 1, off the default (core 0) app/net work.
    xTaskCreatePinnedToCore(capture_task, "capture", 4096, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "capture up: %dx%d RGB565 -> JPEG q%d", CAM_W, CAM_H, CONFIG_DITTO_CAM_JPEG_QUALITY);
    return ESP_OK;
}

bool capture_available(void)
{
    bool ok;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ok = s_jpeg_len > 0;
    xSemaphoreGive(s_lock);
    return ok;
}

esp_err_t capture_get_jpeg(uint8_t **out, size_t *len)
{
    esp_err_t rc = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_jpeg_len > 0) {
        uint8_t *copy = malloc(s_jpeg_len);
        if (copy) {
            memcpy(copy, s_jpeg_buf, s_jpeg_len);
            *out = copy;
            *len = s_jpeg_len;
            rc = ESP_OK;
        } else {
            rc = ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreGive(s_lock);
    return rc;
}

#endif  // CONFIG_DITTO_CAPTURE_ENABLE
