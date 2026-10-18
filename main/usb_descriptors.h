#pragma once
#include <stdint.h>
#include <stdbool.h>

// HID report IDs for the combined keyboard+mouse interface.
enum {
    REPORT_ID_KEYBOARD = 1,
    REPORT_ID_MOUSE,
    REPORT_ID_COUNT,
};

// Install the TinyUSB device stack with Ditto's composite descriptors.
// Blocks only briefly; USB runs on its own TinyUSB task afterwards.
void usb_device_init(void);

// True once the host has configured the device (enumeration complete).
bool usb_device_mounted(void);
