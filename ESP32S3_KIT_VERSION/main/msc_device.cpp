//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   msc_device.cpp
/// @author Petr Vanek
/// TODO: REWORK !!!

#include "msc_device.h"
#include "bsp/esp32_s3_usb_otg.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "esp_log.h"

static const char *TAG = "MSC";

// Card descriptor must outlive this function - TinyUSB keeps the pointer.
static sdmmc_card_t s_card;       
static bool s_cardReady = false;

bool mscDeviceCardReady()
{
    return s_cardReady;
}

/// SD card init WITHOUT esp_vfs_fat mount (BSP's bsp_sdcard_mount() would
/// mount the FAT locally, which must not happen in PC mode). Uses the BSP
/// helpers for the board pinout, mirroring what the BSP mount does inside.
static bool initCardRaw()
{
    sdmmc_host_t host;
    bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &host);

    sdmmc_slot_config_t slot;
    bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &slot);

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "sdmmc_host_init: %s", esp_err_to_name(err));
        return false;
    }
    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "sdmmc_host_init_slot: %s", esp_err_to_name(err));
        return false;
    }
    err = sdmmc_card_init(&host, &s_card);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "sdmmc_card_init: %s (no card?)", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "SD card ready: %s, %llu MB", s_card.cid.name,
             (static_cast<uint64_t>(s_card.csd.capacity) * s_card.csd.sector_size) / (1024 * 1024));
    return true;
}

bool mscDeviceStart()
{
    // Route the OTG controller to the USB DEV connector (the Type-A male that
    // plugs into the PC). The HOST connector stays dead in this boot.
    esp_err_t err = bsp_usb_mode_select_device();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "bsp_usb_mode_select_device: %s", esp_err_to_name(err));
        return false;
    }

    if (!initCardRaw())
    {
        return false;
    }

    // Register the card as MSC backing storage, then start the device stack.
    // mount_config is only used for tinyusb_msc_storage_mount() (local app
    // access), which PC mode never calls.
    const tinyusb_msc_sdmmc_config_t storageCfg = {
        .card = &s_card,
        .callback_mount_changed = nullptr,
        .callback_premount_changed = nullptr,
        .mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 16 * 1024,
        },
    };
    err = tinyusb_msc_storage_init_sdmmc(&storageCfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "msc_storage_init_sdmmc: %s", esp_err_to_name(err));
        return false;
    }

    const tinyusb_config_t tusbCfg = {}; // all-default descriptors from Kconfig
    err = tinyusb_driver_install(&tusbCfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "USB MSC disk exposed to the PC");
    s_cardReady = true;
    return true;
}
