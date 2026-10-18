#include "app_config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "cfg";
static const char *NVS_NS = "ditto";

static char s_token[33];
static char s_ssid[33];
static char s_pass[65];
static char s_host[33];

static void load_str(nvs_handle_t h, const char *key, const char *fallback,
                     char *out, size_t out_sz)
{
    size_t len = out_sz;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        strlcpy(out, fallback ? fallback : "", out_sz);
    }
}

static void gen_token(char out[33])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) out[i] = hex[esp_random() & 0x0F];
    out[32] = '\0';
}

esp_err_t app_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));

    size_t len = sizeof(s_token);
    if (nvs_get_str(h, "token", s_token, &len) != ESP_OK) {
        gen_token(s_token);
        ESP_ERROR_CHECK(nvs_set_str(h, "token", s_token));
        ESP_ERROR_CHECK(nvs_commit(h));
        ESP_LOGW(TAG, "generated new API token");
    }

    load_str(h, "ssid", CONFIG_DITTO_WIFI_SSID, s_ssid, sizeof(s_ssid));
    load_str(h, "pass", CONFIG_DITTO_WIFI_PASSWORD, s_pass, sizeof(s_pass));
    load_str(h, "host", CONFIG_DITTO_HOSTNAME, s_host, sizeof(s_host));

    nvs_close(h);
    ESP_LOGI(TAG, "config loaded (ssid=%s host=%s.local)", s_ssid, s_host);
    return ESP_OK;
}

const char *app_config_token(void)         { return s_token; }
const char *app_config_wifi_ssid(void)     { return s_ssid; }
const char *app_config_wifi_password(void) { return s_pass; }
const char *app_config_hostname(void)      { return s_host; }
