// Turns high-level commands into TinyUSB HID reports. All senders serialize on
// one mutex so concurrent HTTP requests cannot interleave half-reports.
#include "hid_engine.h"
#include "usb_descriptors.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "class/hid/hid.h"
#include "class/hid/hid_device.h"

static const char *TAG = "hid";
static SemaphoreHandle_t s_lock;

// US-layout ASCII -> {needs_shift, keycode}, provided by TinyUSB.
static const uint8_t s_ascii2kc[128][2] = {HID_ASCII_TO_KEYCODE};

#define DEFAULT_KEY_DELAY_MS 8
#define READY_TIMEOUT_MS     500

// Block until the HID endpoint can accept a report, or time out.
static bool wait_ready(void)
{
    if (!usb_device_mounted()) return false;
    for (int waited = 0; waited < READY_TIMEOUT_MS; waited += 2) {
        if (tud_hid_ready()) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

static esp_err_t send_kbd(uint8_t modifier, const uint8_t keycodes[6])
{
    if (!wait_ready()) return ESP_ERR_INVALID_STATE;
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, (uint8_t *)keycodes);
    return ESP_OK;
}

static esp_err_t release_keys(void)
{
    if (!wait_ready()) return ESP_ERR_INVALID_STATE;
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);
    return ESP_OK;
}

void hid_engine_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

esp_err_t hid_type_text(const char *utf8, uint32_t delay_ms)
{
    if (!utf8) return ESP_ERR_INVALID_ARG;
    if (delay_ms == 0) delay_ms = DEFAULT_KEY_DELAY_MS;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    for (const char *p = utf8; *p; ++p) {
        uint8_t c = (uint8_t)*p;
        if (c >= 128) continue;  // non-ASCII skipped until layout support lands
        uint8_t shift = s_ascii2kc[c][0];
        uint8_t kc    = s_ascii2kc[c][1];
        if (kc == 0) continue;

        uint8_t keys[6] = {kc, 0, 0, 0, 0, 0};
        uint8_t mod = shift ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
        if ((err = send_kbd(mod, keys)) != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        if ((err = release_keys()) != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t hid_key_tap(uint8_t modifiers, const uint8_t *keycodes, size_t n)
{
    if (n > 6) return ESP_ERR_INVALID_ARG;
    uint8_t keys[6] = {0};
    for (size_t i = 0; i < n; ++i) keys[i] = keycodes[i];

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = send_kbd(modifiers, keys);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(DEFAULT_KEY_DELAY_MS));
        err = release_keys();
    }
    xSemaphoreGive(s_lock);
    return err;
}

// Case-insensitive table of named keys. Returns a modifier OR a keycode.
esp_err_t hid_keyname_lookup(const char *name, uint8_t *modifier, uint8_t *keycode)
{
    *modifier = 0;
    *keycode = 0;
    if (!name || !*name) return ESP_ERR_INVALID_ARG;

    struct { const char *n; uint8_t mod; uint8_t kc; } map[] = {
        {"ctrl",  KEYBOARD_MODIFIER_LEFTCTRL,  0},
        {"control",KEYBOARD_MODIFIER_LEFTCTRL, 0},
        {"shift", KEYBOARD_MODIFIER_LEFTSHIFT, 0},
        {"alt",   KEYBOARD_MODIFIER_LEFTALT,   0},
        {"gui",   KEYBOARD_MODIFIER_LEFTGUI,   0},
        {"win",   KEYBOARD_MODIFIER_LEFTGUI,   0},
        {"cmd",   KEYBOARD_MODIFIER_LEFTGUI,   0},
        {"enter", 0, HID_KEY_ENTER},
        {"return",0, HID_KEY_ENTER},
        {"esc",   0, HID_KEY_ESCAPE},
        {"escape",0, HID_KEY_ESCAPE},
        {"tab",   0, HID_KEY_TAB},
        {"space", 0, HID_KEY_SPACE},
        {"backspace",0, HID_KEY_BACKSPACE},
        {"delete",0, HID_KEY_DELETE},
        {"del",   0, HID_KEY_DELETE},
        {"insert",0, HID_KEY_INSERT},
        {"home",  0, HID_KEY_HOME},
        {"end",   0, HID_KEY_END},
        {"pageup",0, HID_KEY_PAGE_UP},
        {"pagedown",0, HID_KEY_PAGE_DOWN},
        {"up",    0, HID_KEY_ARROW_UP},
        {"down",  0, HID_KEY_ARROW_DOWN},
        {"left",  0, HID_KEY_ARROW_LEFT},
        {"right", 0, HID_KEY_ARROW_RIGHT},
        {"capslock",0, HID_KEY_CAPS_LOCK},
        {"printscreen",0, HID_KEY_PRINT_SCREEN},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (strcasecmp(name, map[i].n) == 0) {
            *modifier = map[i].mod;
            *keycode = map[i].kc;
            return ESP_OK;
        }
    }
    // Function keys f1..f12
    if ((name[0] == 'f' || name[0] == 'F') && isdigit((unsigned char)name[1])) {
        int fn = atoi(name + 1);
        if (fn >= 1 && fn <= 12) { *keycode = HID_KEY_F1 + (fn - 1); return ESP_OK; }
    }
    // Single printable ASCII char via the layout table.
    if (name[1] == '\0') {
        uint8_t c = (uint8_t)name[0];
        if (c < 128 && s_ascii2kc[c][1]) { *keycode = s_ascii2kc[c][1]; return ESP_OK; }
    }
    ESP_LOGW(TAG, "unknown key name: %s", name);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t hid_mouse_move(int8_t dx, int8_t dy)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = wait_ready() ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) tud_hid_mouse_report(REPORT_ID_MOUSE, 0, dx, dy, 0, 0);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t hid_mouse_scroll(int8_t vertical, int8_t horizontal)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = wait_ready() ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) tud_hid_mouse_report(REPORT_ID_MOUSE, 0, 0, 0, vertical, horizontal);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t hid_mouse_buttons(uint8_t buttons)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = wait_ready() ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, 0, 0, 0, 0);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t hid_mouse_click(uint8_t button)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = wait_ready() ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        tud_hid_mouse_report(REPORT_ID_MOUSE, button, 0, 0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(DEFAULT_KEY_DELAY_MS));
        if (wait_ready()) tud_hid_mouse_report(REPORT_ID_MOUSE, 0, 0, 0, 0, 0);
    }
    xSemaphoreGive(s_lock);
    return err;
}
