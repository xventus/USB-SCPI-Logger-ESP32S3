//
// vim: ts=4 et
// Copyright (c) 2025 Petr Vanek, petr@fotoventus.cz
//
/// @file   wifi_task.cpp
/// @author Petr Vanek

#include "wifi_task.h"
#include "web_server.h"
#include "application.h"
#include "display_task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_littlefs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <cstring>
#include <cstdio>

WifiTask::WifiTask()
{
    _queue = xQueueCreate(4, sizeof(Cmd));
}

WifiTask::~WifiTask()
{
    done();
    if (_queue)
        vQueueDelete(_queue);
}

void WifiTask::toggle()
{
    Cmd c{Cmd::Type::Toggle};
    if (_queue)
        xQueueSend(_queue, &c, 0);
}

bool WifiTask::initOnce()
{
    // NVS - the WiFi driver stores calibration data there.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(LOG_TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return false;
    }

    if (esp_netif_init() != ESP_OK ||
        esp_event_loop_create_default() != ESP_OK)
    {
        ESP_LOGE(LOG_TAG, "netif/event loop init failed");
        return false;
    }
    esp_netif_create_default_wifi_ap(); // kept for the task's lifetime

    // Web page lives in the LittleFS "web" flash partition (works without SD).
    esp_vfs_littlefs_conf_t fs = {};
    fs.base_path = "/web";
    fs.partition_label = "web";
    fs.format_if_mount_failed = false;
    fs.read_only = true;
    err = esp_vfs_littlefs_register(&fs);
    if (err != ESP_OK)
    {
        // Page missing is not fatal - the REST API still works.
        ESP_LOGW(LOG_TAG, "littlefs mount failed: %s (no web page)", esp_err_to_name(err));
    }
    return true;
}

void WifiTask::apStart()
{
    if (_active)
        return;

    wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&initCfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(LOG_TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return;
    }
    _wifiInited = true;

    wifi_config_t ap = {};
    std::strncpy(reinterpret_cast<char *>(ap.ap.ssid), AP_SSID, sizeof(ap.ap.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(ap.ap.password), AP_PASS, sizeof(ap.ap.password) - 1);
    ap.ap.ssid_len = std::strlen(AP_SSID);
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;

    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK ||
        esp_wifi_start() != ESP_OK)
    {
        ESP_LOGE(LOG_TAG, "AP start failed");
        apStop();
        return;
    }

    if (!webServerStart())
    {
        ESP_LOGE(LOG_TAG, "web server start failed");
        apStop();
        return;
    }

    _active = true;
    ESP_LOGI(LOG_TAG, "AP '%s' up, http://%s", AP_SSID, AP_IP);
}

void WifiTask::apStop()
{
    webServerStop();
    if (_wifiInited)
    {
        esp_wifi_stop();
        esp_wifi_deinit(); // releases the WiFi heap
        _wifiInited = false;
    }
    _active = false;
    ESP_LOGI(LOG_TAG, "AP stopped");
}

void WifiTask::loop()
{
    bool ready = initOnce();
    Application::getInstance()->signalTaskStart(Application::TaskBit::Wifi);

    if (!ready)
    {
        while (true)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }

    auto *display = Application::getInstance()->getDisplayTask();

    Cmd c{};
    while (true)
    {
        if (xQueueReceive(_queue, &c, portMAX_DELAY) != pdTRUE)
            continue;

        switch (c.type)
        {
        case Cmd::Type::Toggle:
            if (_active)
                apStop();
            else
                apStart();

            if (display)
            {
                char line[64];
                if (_active)
                    std::snprintf(line, sizeof(line), literals::fmt_ap_on, AP_SSID, AP_IP);
                else
                    std::snprintf(line, sizeof(line), "%s", literals::ui_ap_off);
                display->setStatus(line);
                display->refreshMenu(); // menu WiFi line + icon bar
            }
            break;
        }
    }
}
