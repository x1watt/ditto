// Local TFT status screen (ST7789 over SPI, driven by LVGL via esp_lvgl_port).
// Read-only: it reflects device state and flashes an ACTIVE banner during HID
// injection. Per performance.md, nothing heavy runs on the LVGL task; the refresh
// timer only reads already-computed state and sets label text.
#include "ui.h"
#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ui";

// Set from any task on HID activity; read by the LVGL refresh timer.
static volatile int64_t s_last_activity_us;
#define ACTIVE_WINDOW_US (1500 * 1000)

void ui_note_activity(void) { s_last_activity_us = esp_timer_get_time(); }

#if CONFIG_DITTO_TFT_ENABLE

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_config.h"
#include "usb_descriptors.h"
#include "storage.h"
#include "net_api.h"

#define TFT_HOST    (CONFIG_DITTO_TFT_SPI_HOST == 2 ? SPI3_HOST : SPI2_HOST)
#define TFT_HRES    CONFIG_DITTO_TFT_HRES
#define TFT_VRES    CONFIG_DITTO_TFT_VRES

static lv_obj_t *s_lbl_net, *s_lbl_usb, *s_lbl_disk, *s_lbl_token, *s_banner;

static void refresh_timer(lv_timer_t *t)
{
    (void)t;
    char ip[16];
    net_api_ip(ip, sizeof(ip));
    lv_label_set_text_fmt(s_lbl_net, "%s.local  %s", app_config_hostname(), ip);
    lv_label_set_text_fmt(s_lbl_usb, "USB: %s", usb_device_mounted() ? "connected" : "waiting");
    lv_label_set_text_fmt(s_lbl_disk, "Disk: %s", storage_in_use_by_host() ? "on host" : "firmware");

    bool active = (esp_timer_get_time() - s_last_activity_us) < ACTIVE_WINDOW_US;
    if (active) lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(scr, 8, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Ditto KVM");
    lv_obj_set_style_text_color(title, lv_color_hex(0x4ea1ff), 0);

    s_lbl_net   = lv_label_create(scr);
    s_lbl_usb   = lv_label_create(scr);
    s_lbl_disk  = lv_label_create(scr);
    s_lbl_token = lv_label_create(scr);
    lv_obj_t *white[] = {s_lbl_net, s_lbl_usb, s_lbl_disk, s_lbl_token};
    for (int i = 0; i < 4; ++i)
        lv_obj_set_style_text_color(white[i], lv_color_hex(0xe6e6e6), 0);
    lv_label_set_long_mode(s_lbl_token, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_token, TFT_HRES - 16);
    lv_label_set_text_fmt(s_lbl_token, "token:\n%s", app_config_token());

    // ACTIVE banner: hidden until HID activity, then a loud red bar.
    s_banner = lv_obj_create(scr);
    lv_obj_set_size(s_banner, TFT_HRES - 16, 40);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(0xd21c1c), 0);
    lv_obj_set_style_border_width(s_banner, 0, 0);
    lv_obj_t *bl = lv_label_create(s_banner);
    lv_label_set_text(bl, "HID ACTIVE");
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_center(bl);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
}

static esp_lcd_panel_handle_t panel_init(esp_lcd_panel_io_handle_t *io_out)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_DITTO_TFT_SCLK_GPIO,
        .mosi_io_num = CONFIG_DITTO_TFT_MOSI_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_HRES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(TFT_HOST, &bus, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = CONFIG_DITTO_TFT_DC_GPIO,
        .cs_gpio_num = CONFIG_DITTO_TFT_CS_GPIO,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TFT_HOST, &io_cfg, &io));

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_DITTO_TFT_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
#if CONFIG_DITTO_TFT_INVERT
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

#if CONFIG_DITTO_TFT_BL_GPIO >= 0
    gpio_config_t bl = { .pin_bit_mask = 1ULL << CONFIG_DITTO_TFT_BL_GPIO,
                         .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl);
    gpio_set_level(CONFIG_DITTO_TFT_BL_GPIO, 1);
#endif
    *io_out = io;
    return panel;
}

void ui_init(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = panel_init(&io);

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = TFT_HRES * 40,
        .double_buffer = true,
        .hres = TFT_HRES,
        .vres = TFT_VRES,
        .flags = { .buff_dma = true },
    };
    lvgl_port_add_disp(&disp_cfg);

    // Widget creation must hold the LVGL lock (we are not on the port task).
    if (lvgl_port_lock(0)) {
        build_screen();
        lv_timer_create(refresh_timer, 500, NULL);
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "TFT status UI up (%dx%d)", TFT_HRES, TFT_VRES);
}

#else  // CONFIG_DITTO_TFT_ENABLE

void ui_init(void) { ESP_LOGI(TAG, "TFT disabled"); }

#endif
