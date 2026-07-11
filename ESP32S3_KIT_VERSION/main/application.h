//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   application.h
/// @author Petr Vanek

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <vector>
#include <stdio.h>

#include "literals.h"
#include "display_task.h"
#include "storage_task.h"
#include "instrument_task.h"
#include "wifi_task.h"

/**
 * @brief application class - singleton
 */
class Application
{
public:
    enum class TaskBit : uint32_t
    {
        Display = (1 << 0),    ///< DisplayTask (LVGL UI)
        Storage = (1 << 1),    ///< StorageTask (SD / CSV)
        Instrument = (1 << 2), ///< InstrumentTask (USB host + USBTMC)
        Wifi = (1 << 3),       ///< WifiTask (on-demand Soft AP + web)
    };

    virtual ~Application();

    /// application initialization (core services)
    void init();

    /// create and start the tasks
    void run();

    /// application deinitialization
    void done();

    void waitForAllTasks(const std::vector<TaskBit> &tasks);
    void signalTaskStart(TaskBit task);

    DisplayTask *getDisplayTask() { return &_displayTask; }
    StorageTask *getStorageTask() { return &_storageTask; }
    InstrumentTask *getInstrumentTask() { return &_instrumentTask; }
    WifiTask *getWifiTask() { return &_wifiTask; }

    /// Singleton
    Application *operator->();
    Application *operator->() const;
    static Application *getInstance();

private:
    Application();
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    DisplayTask _displayTask;       ///< ST7789 LVGL UI
    StorageTask _storageTask;       ///< SD card / CSV writer
    InstrumentTask _instrumentTask; ///< USB host, enumeration, USBTMC
    WifiTask _wifiTask;             ///< on-demand Soft AP + web server
    EventGroupHandle_t _taskEventGroup{nullptr};

    static constexpr const char *TAG = "APP";
};
