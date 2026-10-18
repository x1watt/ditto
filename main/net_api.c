// LAN-facing control plane: Wi-Fi STA + mDNS + HTTP API. Every handler requires
// a matching bearer token. Heavy HID work runs inline in the httpd worker, which
// is acceptable for the short command strings this API carries.
#include "net_api.h"
#include "app_config.h"
#include "hid_engine.h"
#include "usb_descriptors.h"
#include "storage.h"
#include "ui.h"
#include "capture.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "cJSON.h"

static const char *TAG = "net";
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
static esp_netif_t *s_netif;
static httpd_handle_t s_srv;

// Live-stream client registry (one WebSocket per connected viewer).
#define STREAM_MAX_CLIENTS 4
#define STREAM_MAX_FPS     5
static int s_ws_fds[STREAM_MAX_CLIENTS];
static SemaphoreHandle_t s_ws_lock;

static void ws_add_client(int fd)
{
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < STREAM_MAX_CLIENTS; ++i) {
        if (s_ws_fds[i] == fd) break;           // already present
        if (s_ws_fds[i] == 0) { s_ws_fds[i] = fd; break; }
    }
    xSemaphoreGive(s_ws_lock);
}

static void ws_drop_client(int fd)
{
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < STREAM_MAX_CLIENTS; ++i)
        if (s_ws_fds[i] == fd) s_ws_fds[i] = 0;
    xSemaphoreGive(s_ws_lock);
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "wifi disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, app_config_wifi_ssid(), sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, app_config_wifi_password(), sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void net_api_ip(char *buf, size_t n)
{
    esp_netif_ip_info_t ip = {0};
    if (s_netif) esp_netif_get_ip_info(s_netif, &ip);
    snprintf(buf, n, IPSTR, IP2STR(&ip.ip));
}

static void mdns_start(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(app_config_hostname()));
    mdns_instance_name_set("Ditto KVM");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
static bool authorized(httpd_req_t *req)
{
    char hdr[80] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    const char *p = hdr;
    if (strncasecmp(p, "Bearer ", 7) == 0) p += 7;
    const char *tok = app_config_token();
    // Constant-time compare to avoid leaking length/prefix via timing.
    size_t n = strlen(tok);
    if (strlen(p) != n) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= (uint8_t)(p[i] ^ tok[i]);
    return diff == 0;
}

static esp_err_t deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return ESP_OK;
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_sz)
{
    size_t total = req->content_len;
    if (total >= buf_sz) return ESP_ERR_INVALID_SIZE;
    size_t off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, buf + off, total - off);
        if (r <= 0) return ESP_FAIL;
        off += r;
    }
    buf[total] = '\0';
    return ESP_OK;
}

static esp_err_t reply_result(httpd_req_t *req, esp_err_t rc)
{
    httpd_resp_set_type(req, "application/json");
    if (rc == ESP_OK) {
        ui_note_activity();
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else if (rc == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"usb not connected\"}");
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad request\"}");
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
static esp_err_t h_status(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);
    esp_netif_ip_info_t ip = {0};
    if (s_netif) esp_netif_get_ip_info(s_netif, &ip);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "usb_mounted", usb_device_mounted());
    char ipstr[16];
    snprintf(ipstr, sizeof(ipstr), IPSTR, IP2STR(&ip.ip));
    cJSON_AddStringToObject(root, "ip", ipstr);
    cJSON_AddStringToObject(root, "hostname", app_config_hostname());
    cJSON_AddBoolToObject(root, "disk_on_host", storage_in_use_by_host());
    cJSON_AddBoolToObject(root, "capture_ready", capture_available());
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    cJSON_free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t h_type(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);
    char buf[2048];
    if (read_body(req, buf, sizeof(buf)) != ESP_OK) return reply_result(req, ESP_FAIL);
    cJSON *root = cJSON_Parse(buf);
    if (!root) return reply_result(req, ESP_FAIL);

    const cJSON *text  = cJSON_GetObjectItem(root, "text");
    const cJSON *delay = cJSON_GetObjectItem(root, "delayMs");
    esp_err_t rc = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(text)) {
        uint32_t d = cJSON_IsNumber(delay) ? (uint32_t)delay->valueint : 0;
        rc = hid_type_text(text->valuestring, d);
    }
    cJSON_Delete(root);
    return reply_result(req, rc);
}

static esp_err_t h_key(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);
    char buf[512];
    if (read_body(req, buf, sizeof(buf)) != ESP_OK) return reply_result(req, ESP_FAIL);
    cJSON *root = cJSON_Parse(buf);
    if (!root) return reply_result(req, ESP_FAIL);

    const cJSON *keys = cJSON_GetObjectItem(root, "keys");
    esp_err_t rc = ESP_ERR_INVALID_ARG;
    if (cJSON_IsArray(keys)) {
        uint8_t mod = 0, kc[6] = {0};
        size_t nk = 0;
        rc = ESP_OK;
        const cJSON *it;
        cJSON_ArrayForEach(it, keys) {
            if (!cJSON_IsString(it)) { rc = ESP_ERR_INVALID_ARG; break; }
            uint8_t m, k;
            if (hid_keyname_lookup(it->valuestring, &m, &k) != ESP_OK) { rc = ESP_ERR_INVALID_ARG; break; }
            mod |= m;
            if (k) {
                if (nk >= 6) { rc = ESP_ERR_INVALID_ARG; break; }
                kc[nk++] = k;
            }
        }
        if (rc == ESP_OK) rc = hid_key_tap(mod, kc, nk);
    }
    cJSON_Delete(root);
    return reply_result(req, rc);
}

static uint8_t button_from(const char *name)
{
    if (!name) return DITTO_MB_LEFT;
    if (strcasecmp(name, "right") == 0)  return DITTO_MB_RIGHT;
    if (strcasecmp(name, "middle") == 0) return DITTO_MB_MIDDLE;
    return DITTO_MB_LEFT;
}

static esp_err_t h_mouse(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);
    char buf[256];
    if (read_body(req, buf, sizeof(buf)) != ESP_OK) return reply_result(req, ESP_FAIL);
    cJSON *root = cJSON_Parse(buf);
    if (!root) return reply_result(req, ESP_FAIL);

    esp_err_t rc = ESP_ERR_INVALID_ARG;
    const cJSON *dx = cJSON_GetObjectItem(root, "dx");
    const cJSON *dy = cJSON_GetObjectItem(root, "dy");
    const cJSON *scroll = cJSON_GetObjectItem(root, "scroll");
    const cJSON *click = cJSON_GetObjectItem(root, "click");
    const cJSON *button = cJSON_GetObjectItem(root, "button");
    const cJSON *action = cJSON_GetObjectItem(root, "action");

    if (cJSON_IsNumber(dx) || cJSON_IsNumber(dy)) {
        rc = hid_mouse_move(cJSON_IsNumber(dx) ? (int8_t)dx->valueint : 0,
                            cJSON_IsNumber(dy) ? (int8_t)dy->valueint : 0);
    } else if (cJSON_IsObject(scroll)) {
        const cJSON *v = cJSON_GetObjectItem(scroll, "v");
        const cJSON *h = cJSON_GetObjectItem(scroll, "h");
        rc = hid_mouse_scroll(cJSON_IsNumber(v) ? (int8_t)v->valueint : 0,
                              cJSON_IsNumber(h) ? (int8_t)h->valueint : 0);
    } else if (cJSON_IsString(click)) {
        rc = hid_mouse_click(button_from(click->valuestring));
    } else if (cJSON_IsString(button) && cJSON_IsString(action)) {
        uint8_t b = button_from(button->valuestring);
        rc = hid_mouse_buttons(strcasecmp(action->valuestring, "down") == 0 ? b : 0);
    }
    cJSON_Delete(root);
    return reply_result(req, rc);
}

static esp_err_t h_screenshot(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);
    uint8_t *jpg = NULL;
    size_t len = 0;
    esp_err_t rc = capture_get_jpeg(&jpg, &len);
    if (rc == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "501 Not Implemented");
        httpd_resp_sendstr(req, "{\"error\":\"capture disabled\"}");
        return ESP_OK;
    }
    if (rc != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"error\":\"no frame yet\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    esp_err_t send = httpd_resp_send(req, (const char *)jpg, len);
    free(jpg);
    return send;
}

// WebSocket handshake auth: accept the bearer header, or a ?token= query param
// (a browser cannot set Authorization on a WS upgrade; our clients can use either).
static bool ws_authorized(httpd_req_t *req)
{
    if (authorized(req)) return true;
    char q[160];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    char val[48] = {0};
    if (httpd_query_key_value(q, "token", val, sizeof(val)) != ESP_OK) return false;
    const char *tok = app_config_token();
    size_t n = strlen(tok);
    if (strlen(val) != n) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= (uint8_t)(val[i] ^ tok[i]);
    return diff == 0;
}

static esp_err_t h_stream(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {               // the upgrade handshake
        int fd = httpd_req_to_sockfd(req);
        if (!ws_authorized(req)) {
            httpd_sess_trigger_close(s_srv, fd);
            return ESP_OK;
        }
        ws_add_client(fd);
        ESP_LOGI(TAG, "stream client fd=%d", fd);
        return ESP_OK;
    }
    // Client -> server frame: we only care about close/ping housekeeping.
    httpd_ws_frame_t f = {0};
    esp_err_t r = httpd_ws_recv_frame(req, &f, 0);
    if (r != ESP_OK) return r;
    if (f.type == HTTPD_WS_TYPE_CLOSE) ws_drop_client(httpd_req_to_sockfd(req));
    return ESP_OK;
}

// Pushes the latest JPEG to every stream client at a capped frame rate.
static void stream_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(1000 / STREAM_MAX_FPS);
    while (1) {
        vTaskDelay(period);
        bool any = false;
        for (int i = 0; i < STREAM_MAX_CLIENTS; ++i) if (s_ws_fds[i]) any = true;
        if (!any) continue;

        uint8_t *jpg = NULL;
        size_t len = 0;
        if (capture_get_jpeg(&jpg, &len) != ESP_OK) continue;

        httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_BINARY, .payload = jpg, .len = len };
        for (int i = 0; i < STREAM_MAX_CLIENTS; ++i) {
            int fd = s_ws_fds[i];
            if (!fd) continue;
            if (httpd_ws_get_fd_info(s_srv, fd) != HTTPD_WS_CLIENT_WEBSOCKET ||
                httpd_ws_send_frame_async(s_srv, fd, &f) != ESP_OK) {
                ws_drop_client(fd);
            }
        }
        free(jpg);
    }
}

// ---------------------------------------------------------------------------
// File exchange (milestone 7)
//
// The USB host and the firmware must never write the shared FAT at the same time
// (see docs/architecture.md). So file I/O requires the firmware to own the disk:
// POST /msc {"action":"eject"} takes it from the host (app-mount), "attach" gives
// it back. /file* refuse with 409 while the host still owns the disk.
// ---------------------------------------------------------------------------

// Accept only a safe basename (no path separators, no "..", non-empty).
static bool safe_name(const char *n)
{
    if (!n || !*n || strlen(n) > 64) return false;
    if (strstr(n, "..")) return false;
    for (const char *p = n; *p; ++p)
        if (*p == '/' || *p == '\\') return false;
    return true;
}

// Pull ?path= from the query into name (basename only).
static bool query_name(httpd_req_t *req, char *name, size_t n)
{
    char q[128];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, "path", name, n) != ESP_OK) return false;
    return safe_name(name);
}

static esp_err_t json_msg(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

static esp_err_t h_msc(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);
    char buf[128];
    if (read_body(req, buf, sizeof(buf)) != ESP_OK) return json_msg(req, "400 Bad Request", "{\"error\":\"bad body\"}");
    cJSON *root = cJSON_Parse(buf);
    const cJSON *action = root ? cJSON_GetObjectItem(root, "action") : NULL;
    esp_err_t rc = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(action)) {
        if (strcasecmp(action->valuestring, "eject") == 0)       rc = storage_app_mount();    // host loses disk
        else if (strcasecmp(action->valuestring, "attach") == 0) rc = storage_app_unmount();  // host regains disk
    }
    cJSON_Delete(root);
    if (rc == ESP_OK)
        return json_msg(req, "200 OK", storage_in_use_by_host()
                        ? "{\"ok\":true,\"owner\":\"host\"}" : "{\"ok\":true,\"owner\":\"firmware\"}");
    return json_msg(req, "400 Bad Request", "{\"ok\":false,\"error\":\"action must be eject|attach\"}");
}

static esp_err_t h_file_put(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);
    if (storage_in_use_by_host())
        return json_msg(req, "409 Conflict", "{\"error\":\"disk is on host; POST /msc {\\\"action\\\":\\\"eject\\\"} first\"}");
    char name[72];
    if (!query_name(req, name, sizeof(name)))
        return json_msg(req, "400 Bad Request", "{\"error\":\"missing/invalid ?path=\"}");

    char path[300];
    snprintf(path, sizeof(path), DITTO_DISK_PATH "/%.64s", name);
    FILE *f = fopen(path, "wb");
    if (!f) return json_msg(req, "500 Internal Server Error", "{\"error\":\"open failed\"}");

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining);
        if (r <= 0) { fclose(f); return json_msg(req, "400 Bad Request", "{\"error\":\"recv failed\"}"); }
        if (fwrite(buf, 1, r, f) != (size_t)r) { fclose(f); return json_msg(req, "507 Insufficient Storage", "{\"error\":\"write failed\"}"); }
        remaining -= r;
    }
    fclose(f);
    return json_msg(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_file_get(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);
    if (storage_in_use_by_host())
        return json_msg(req, "409 Conflict", "{\"error\":\"disk is on host; eject first\"}");
    char name[72];
    if (!query_name(req, name, sizeof(name)))
        return json_msg(req, "400 Bad Request", "{\"error\":\"missing/invalid ?path=\"}");

    char path[300];
    snprintf(path, sizeof(path), DITTO_DISK_PATH "/%.64s", name);
    FILE *f = fopen(path, "rb");
    if (!f) return json_msg(req, "404 Not Found", "{\"error\":\"no such file\"}");

    httpd_resp_set_type(req, "application/octet-stream");
    char buf[1024];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) { fclose(f); return ESP_FAIL; }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);  // terminate
    return ESP_OK;
}

static esp_err_t h_files(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);
    if (storage_in_use_by_host())
        return json_msg(req, "409 Conflict", "{\"error\":\"disk is on host; eject first\"}");

    DIR *d = opendir(DITTO_DISK_PATH);
    if (!d) return json_msg(req, "500 Internal Server Error", "{\"error\":\"opendir failed\"}");
    cJSON *arr = cJSON_CreateArray();
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type == DT_DIR) continue;
        char path[300];
        snprintf(path, sizeof(path), DITTO_DISK_PATH "/%.200s", e->d_name);
        struct stat st = {0};
        stat(path, &st);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->d_name);
        cJSON_AddNumberToObject(o, "size", st.st_size);
        cJSON_AddItemToArray(arr, o);
    }
    closedir(d);
    char *out = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    cJSON_free(out);
    cJSON_Delete(arr);
    return ESP_OK;
}

static void register_routes(httpd_handle_t srv)
{
    const httpd_uri_t routes[] = {
        {.uri = "/status",     .method = HTTP_GET,  .handler = h_status},
        {.uri = "/type",       .method = HTTP_POST, .handler = h_type},
        {.uri = "/key",        .method = HTTP_POST, .handler = h_key},
        {.uri = "/mouse",      .method = HTTP_POST, .handler = h_mouse},
        {.uri = "/screenshot", .method = HTTP_GET,  .handler = h_screenshot},
        {.uri = "/stream",     .method = HTTP_GET,  .handler = h_stream, .is_websocket = true},
        {.uri = "/msc",        .method = HTTP_POST, .handler = h_msc},
        {.uri = "/file",       .method = HTTP_POST, .handler = h_file_put},
        {.uri = "/file",       .method = HTTP_GET,  .handler = h_file_get},
        {.uri = "/files",      .method = HTTP_GET,  .handler = h_files},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
        httpd_register_uri_handler(srv, &routes[i]);
}

void net_api_start(void)
{
    wifi_start();
    mdns_start();

    s_ws_lock = xSemaphoreCreateMutex();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&s_srv, &cfg));
    register_routes(s_srv);

    // Broadcaster pinned to core 1, alongside the capture/encode work.
    xTaskCreatePinnedToCore(stream_task, "stream", 4096, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "HTTP API listening on :80 (+ WS /stream)");
}
