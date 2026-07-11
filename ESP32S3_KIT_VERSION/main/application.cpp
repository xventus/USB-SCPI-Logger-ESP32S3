//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   application.cpp
/// @author Petr Vanek

#include "application.h"
#include "usb_mode.h"
#include "msc_device.h"
#include "esp_log.h"
#include <inttypes.h>

// global application instance as a singleton

Application *Application::getInstance()
{
    static Application instance;
    return &instance;
}

Application::Application() : _taskEventGroup(nullptr)
{
}

Application::~Application()
{
    done();
    if (_taskEventGroup)
    {
        vEventGroupDelete(_taskEventGroup);
    }
}

void Application::init()
{
    // Core service initialization shared by all task wrappers.
    _taskEventGroup = xEventGroupCreate();
    if (!_taskEventGroup)
    {
        ESP_LOGE(TAG, "Failed to create event group!");
        return;
    }
}

void Application::run()
{
    // PC mode (USB disk): only the display comes up - no USB host, no
    // StorageTask (the PC owns the raw SD blocks, see msc_device.h). The
    // display handles the UP button = reboot back to logger.
    if (usbmode::isPcMode())
    {
        ESP_LOGI(TAG, "booting in PC mode (USB MSC disk)");
        if (!_displayTask.init(literals::tsk_display, tskIDLE_PRIORITY + 2ul, 6144))
            return;
        waitForAllTasks({TaskBit::Display});

        _displayTask.setStatus(mscDeviceStart() ? literals::ui_pc_mode
                                                : literals::ui_pc_mode_failed);
        _displayTask.setValue(literals::ui_pc_back);
        return;
    }

    do
    {
        // The display and storage tasks must come up first - the instrument
        // task pushes status/IDN to the display and samples to storage.
        if (!_displayTask.init(literals::tsk_display, tskIDLE_PRIORITY + 2ul, 6144))
            break;

        if (!_storageTask.init(literals::tsk_storage, tskIDLE_PRIORITY + 2ul, 6144))
            break;

        if (!_instrumentTask.init(literals::tsk_instrument, tskIDLE_PRIORITY + 3ul, 6144))
            break;

        if (!_wifiTask.init(literals::tsk_wifi, tskIDLE_PRIORITY + 1ul, 6144))
            break;

        waitForAllTasks({TaskBit::Display, TaskBit::Storage, TaskBit::Instrument, TaskBit::Wifi});

        ESP_LOGI(TAG, "All tasks initialized. Proceeding...");

    } while (false);
}

void Application::done()
{
}

Application *Application::operator->()
{
    return Application::getInstance();
}

Application *Application::operator->() const
{
    return Application::getInstance();
}

void Application::signalTaskStart(TaskBit task)
{
    if (_taskEventGroup)
    {
        xEventGroupSetBits(_taskEventGroup, static_cast<EventBits_t>(task));
        ESP_LOGI(TAG, "Task %" PRIu32 " signaled start", static_cast<uint32_t>(task));
    }
}

void Application::waitForAllTasks(const std::vector<TaskBit> &tasks)
{
    if (!_taskEventGroup)
    {
        ESP_LOGE(TAG, "Event group not initialized!");
        return;
    }

    EventBits_t allBits = 0;
    for (const auto &task : tasks)
    {
        allBits |= static_cast<EventBits_t>(task);
    }

    EventBits_t bits = xEventGroupWaitBits(_taskEventGroup, allBits, pdFALSE, pdTRUE, portMAX_DELAY);

    if ((bits & allBits) == allBits)
    {
        ESP_LOGI(TAG, "All tasks have started successfully!");
    }
    else
    {
        ESP_LOGE(TAG, "Some tasks failed to start!");
    }
}
