//
// vim: ts=4 et
// Copyright (c) 2025 Petr Vanek, petr@fotoventus.cz
//
/// @file   wifi_task.h
/// @author Petr Vanek
///
/// On-demand WiFi Soft AP + web server

#pragma once

#include "rptask.h"

class WifiTask : public RPTask
{
public:
    // Soft AP credentials (WPA2; password must be >= 8 chars).
    static constexpr const char *AP_SSID = "SCPI-LOGGER";
    static constexpr const char *AP_PASS = "scpi-logger";
    static constexpr const char *AP_IP = "192.168.4.1"; ///< esp_netif default

    WifiTask();
    virtual ~WifiTask();

    /// Toggle the AP on/off (from the DOWN button / menu). Non-blocking.
    void toggle();

    /// True while the Soft AP (and the web server) is running.
    bool apActive() const { return _active; }

protected:
    void loop() override;

private:
    static constexpr const char *LOG_TAG = "WIFI";

    struct Cmd
    {
        enum class Type : uint8_t
        {
            Toggle,
        } type;
    };

    bool initOnce(); ///< NVS + netif + event loop + LittleFS mount
    void apStart();
    void apStop();

    QueueHandle_t _queue{nullptr};
    volatile bool _active{false};
    bool _wifiInited{false}; ///< esp_wifi_init done (paired with deinit)
};
