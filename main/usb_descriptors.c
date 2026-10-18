// TinyUSB composite device: a single HID interface carrying both a keyboard
// and a mouse (distinguished by report ID). MSC is added in milestone 2.
#include "usb_descriptors.h"

#include "esp_log.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

static const char *TAG = "usb";

// ---------------------------------------------------------------------------
// HID report descriptor: keyboard (report id 1) + relative mouse (report id 2).
// Absolute/digitizer mouse for click-at-coordinate is a milestone-5 enhancement.
// ---------------------------------------------------------------------------
static const uint8_t hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
};

// ---------------------------------------------------------------------------
// Device descriptor. VID 0x303A is Espressif's; PID is app-local.
// ---------------------------------------------------------------------------
static const tusb_desc_device_t device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x4004,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

// ---------------------------------------------------------------------------
// Configuration descriptor: one HID interface, IN endpoint 0x81.
// ---------------------------------------------------------------------------
enum {
    ITF_NUM_HID = 0,
    ITF_NUM_MSC,
    ITF_NUM_TOTAL,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN)
#define EPNUM_HID     0x81
#define EPNUM_MSC_OUT 0x02
#define EPNUM_MSC_IN  0x82

static const uint8_t config_desc[] = {
    // config number, interface count, string index, total length, attributes, power (mA)
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    // interface number, string index, protocol, report descriptor len, EP IN addr, size, polling interval (ms)
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_NONE,
                       sizeof(hid_report_desc), EPNUM_HID,
                       CFG_TUD_HID_EP_BUFSIZE, 5),
    // interface number, string index, EP OUT, EP IN, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

static const char *string_desc[] = {
    (const char[]){0x09, 0x04},  // 0: supported language = English (0x0409)
    "x1watt",                    // 1: manufacturer
    "Ditto KVM",                 // 2: product
    "ditto-0001",                // 3: serial
    "Ditto HID",                 // 4: HID interface
    "Ditto Disk",                // 5: MSC interface
};

// ---------------------------------------------------------------------------
// TinyUSB mandatory callbacks.
// ---------------------------------------------------------------------------
const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;  // no feature/input reports fetched by host
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           const uint8_t *buffer, uint16_t bufsize)
{
    // Host->device output reports (e.g. keyboard LED state). Ignored for now.
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

void usb_device_init(void)
{
    const tinyusb_config_t cfg = {
        .device_descriptor        = &device_desc,
        .string_descriptor        = string_desc,
        .string_descriptor_count  = sizeof(string_desc) / sizeof(string_desc[0]),
        .external_phy             = false,
        .configuration_descriptor = config_desc,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    ESP_LOGI(TAG, "USB composite HID installed");
}

bool usb_device_mounted(void)
{
    return tud_mounted();
}
