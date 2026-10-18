#pragma once
#include <stdbool.h>
#include "esp_err.h"

#define DITTO_DISK_PATH "/disk"

// Bring up the MSC backing store (SPI-flash partition or microSD per menuconfig)
// and register it with TinyUSB. After this the target sees a USB drive.
esp_err_t storage_init(void);

// M7 handoff helpers: take the medium away from the USB host so the firmware can
// write to it over the LAN, then give it back. While app-mounted the host cannot
// read/write the disk (prevents concurrent-write corruption).
esp_err_t storage_app_mount(void);     // host loses the drive; FS at DITTO_DISK_PATH
esp_err_t storage_app_unmount(void);   // host regains the drive
bool      storage_in_use_by_host(void);
