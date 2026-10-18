// USB mass-storage backing store. Two backends chosen at build time:
//   - SPI-flash: the 'storage' FAT partition, no extra hardware.
//   - microSD:   SDMMC 1-line slot with menuconfig pins.
// The esp_tinyusb MSC component owns the tud_msc_* callbacks; this file only
// provides the block device and the app-side mount/unmount for the LAN handoff.
#include "storage.h"

#include "esp_log.h"
#include "esp_check.h"
#include "sdkconfig.h"

#include "tusb_msc_storage.h"
#include "esp_vfs_fat.h"

#if CONFIG_DITTO_MSC_SDMMC
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
static sdmmc_card_t *s_card;
#else
#include "wear_levelling.h"
#include "esp_partition.h"
static wl_handle_t s_wl = WL_INVALID_HANDLE;
#endif

static const char *TAG = "storage";

static const esp_vfs_fat_mount_config_t k_mount_cfg = {
    .format_if_mount_failed = true,
    .max_files = 4,
    .allocation_unit_size = 4096,
};

esp_err_t storage_init(void)
{
#if CONFIG_DITTO_MSC_SDMMC
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = CONFIG_DITTO_SD_CLK_GPIO;
    slot.cmd = CONFIG_DITTO_SD_CMD_GPIO;
    slot.d0  = CONFIG_DITTO_SD_D0_GPIO;

    ESP_RETURN_ON_ERROR(sdmmc_host_init(), TAG, "sdmmc host");
    ESP_RETURN_ON_ERROR(sdmmc_host_init_slot(host.slot, &slot), TAG, "sdmmc slot");
    s_card = calloc(1, sizeof(sdmmc_card_t));
    ESP_RETURN_ON_FALSE(s_card, ESP_ERR_NO_MEM, TAG, "card alloc");
    ESP_RETURN_ON_ERROR(sdmmc_card_init(&host, s_card), TAG, "card init");

    const tinyusb_msc_sdmmc_config_t cfg = { .card = s_card, .mount_config = k_mount_cfg };
    ESP_RETURN_ON_ERROR(tinyusb_msc_storage_init_sdmmc(&cfg), TAG, "msc sdmmc init");
    ESP_LOGI(TAG, "MSC backend: microSD");
#else
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    ESP_RETURN_ON_FALSE(part, ESP_ERR_NOT_FOUND, TAG, "no 'storage' partition");
    ESP_RETURN_ON_ERROR(wl_mount(part, &s_wl), TAG, "wl_mount");

    const tinyusb_msc_spiflash_config_t cfg = { .wl_handle = s_wl, .mount_config = k_mount_cfg };
    ESP_RETURN_ON_ERROR(tinyusb_msc_storage_init_spiflash(&cfg), TAG, "msc spiflash init");
    ESP_LOGI(TAG, "MSC backend: SPI flash 'storage' partition");
#endif

    // Format-if-needed once so the host sees a valid FAT volume on first plug,
    // then hand the medium to the USB host.
    esp_err_t err = tinyusb_msc_storage_mount(DITTO_DISK_PATH);
    if (err == ESP_OK) tinyusb_msc_storage_unmount();

    ESP_LOGI(TAG, "MSC ready: %lu sectors x %lu bytes",
             (unsigned long)tinyusb_msc_storage_get_sector_count(),
             (unsigned long)tinyusb_msc_storage_get_sector_size());
    return ESP_OK;
}

esp_err_t storage_app_mount(void)   { return tinyusb_msc_storage_mount(DITTO_DISK_PATH); }
esp_err_t storage_app_unmount(void) { return tinyusb_msc_storage_unmount(); }
bool      storage_in_use_by_host(void) { return tinyusb_msc_storage_in_use_by_usb_host(); }
