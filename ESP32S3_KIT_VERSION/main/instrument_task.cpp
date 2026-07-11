//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   instrument_task.cpp
/// @author Petr Vanek

#include "instrument_task.h"
#include "display_task.h"
#include "storage_task.h"
#include "application.h"
#include "hardware.h"
#include "bsp/esp32_s3_usb_otg.h"
#include "esp_log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

InstrumentTask::InstrumentTask()
{
    _cmdQueue = xQueueCreate(8, sizeof(Cmd));
    _uiMutex = xSemaphoreCreateMutex();
}

InstrumentTask::~InstrumentTask()
{
    done();
    if (_client)
    {
        usb_host_client_deregister(_client);
        _client = nullptr;
    }
    if (_cmdQueue)
        vQueueDelete(_cmdQueue);
    if (_uiMutex)
        vSemaphoreDelete(_uiMutex);
}

// --- Control API -----------------------------------------------------------

void InstrumentTask::cycleInterval()
{
    Cmd c{Cmd::Type::CycleInterval};
    if (_cmdQueue)
        xQueueSend(_cmdQueue, &c, 0);
}

void InstrumentTask::cycleQuantity()
{
    Cmd c{Cmd::Type::CycleQuantity};
    if (_cmdQueue)
        xQueueSend(_cmdQueue, &c, 0);
}

void InstrumentTask::toggleRunning()
{
    Cmd c{Cmd::Type::ToggleRunning};
    if (_cmdQueue)
        xQueueSend(_cmdQueue, &c, 0);
}

void InstrumentTask::setRunning(bool run)
{
    Cmd c{Cmd::Type::SetRunning, run};
    if (_cmdQueue)
        xQueueSend(_cmdQueue, &c, 0);
}

void InstrumentTask::restartLogging()
{
    Cmd c{Cmd::Type::RestartSession};
    if (_cmdQueue)
        xQueueSend(_cmdQueue, &c, 0);
}

uint32_t InstrumentTask::sessionSeconds() const
{
    if (!_measuring)
        return 0;
    return (xTaskGetTickCount() - _measStartTick) * portTICK_PERIOD_MS / 1000;
}

void InstrumentTask::getLiveValues(LiveValues &out) const
{
    if (_uiMutex && xSemaphoreTake(_uiMutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        out = _live;
        xSemaphoreGive(_uiMutex);
    }
    else
    {
        out = LiveValues{};
    }
}

void InstrumentTask::getUiState(UiState &out) const
{
    if (_uiMutex && xSemaphoreTake(_uiMutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        out = _ui;
        xSemaphoreGive(_uiMutex);
    }
    else
    {
        out = UiState{};
    }
}

void InstrumentTask::publishUi()
{
    if (!_uiMutex || xSemaphoreTake(_uiMutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return;

    _ui = UiState{};
    _ui.intervalMs = _intervalMs;
    _ui.running = _running;
    _ui.deviceReady = _deviceReady;
    std::strncpy(_ui.idn, _idn, sizeof(_ui.idn) - 1);
    if (!_deviceReady)
        _live = LiveValues{}; // stale readings vanish with the instrument
    if (_profile)
    {
        std::strncpy(_ui.profileName, _profile->name, sizeof(_ui.profileName) - 1);
        _ui.measCount = _profile->measCount;
        _ui.activeMeas = _activeMeas;
        _ui.selectSingle = _profile->selectSingle;
        if (_activeMeas < _profile->measCount)
        {
            std::strncpy(_ui.activeLabel, _profile->meas[_activeMeas].label, sizeof(_ui.activeLabel) - 1);
            std::strncpy(_ui.activeUnit, _profile->meas[_activeMeas].unit, sizeof(_ui.activeUnit) - 1);
        }
    }
    xSemaphoreGive(_uiMutex);

    // The menu renders from this snapshot; tell it the state really changed
    // (commands run async - painting on the button press would lag by one).
    if (_display)
        _display->refreshMenu();
}

// --- USB host --------------------------------------------------------------

bool InstrumentTask::startHost()
{
    esp_err_t err = bsp_usb_host_start(HW_USB_HOST_POWER_MODE, HW_USB_HOST_LIMIT_500MA);
    if (err != ESP_OK)
    {
        ESP_LOGE(LOG_TAG, "bsp_usb_host_start failed: %s", esp_err_to_name(err));
        return false;
    }

    usb_host_client_config_t clientCfg = {};
    clientCfg.is_synchronous = false;
    clientCfg.max_num_event_msg = 5;
    clientCfg.async.client_event_callback = &InstrumentTask::clientEventCb;
    clientCfg.async.callback_arg = this;

    err = usb_host_client_register(&clientCfg, &_client);
    if (err != ESP_OK)
    {
        ESP_LOGE(LOG_TAG, "usb_host_client_register failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(LOG_TAG, "USB host started, VBUS on, client registered");
    return true;
}

void InstrumentTask::clientEventCb(const usb_host_client_event_msg_t *event, void *arg)
{
    auto *self = static_cast<InstrumentTask *>(arg);
    switch (event->event)
    {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        self->_pendingAddress = event->new_dev.address;
        self->_pendingConnect = true;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        self->_pendingDisconnect = true;
        break;
    default:
        break;
    }
}

// --- Profile selection -----------------------------------------------------

void InstrumentTask::selectProfile(const char *idn)
{
    _profile = nullptr;

    // SD profiles take precedence over the built-ins
    if (profileLoadFromSd(PROFILE_DIR, idn, _session.vid, _profileBuf))
    {
        _profile = &_profileBuf;
    }
    else
    {
        _profile = profileMatchBuiltin(idn, _session.vid);
    }

    if (!_profile)
    {
        ESP_LOGW(LOG_TAG, "no profile for this instrument");
        if (_display)
        {
            _display->setStatus(idn);
            _display->setValue(literals::ui_no_profile);
        }
        return;
    }

    ESP_LOGI(LOG_TAG, "profile: %s (%u measurement%s, interval %lu ms)",
             _profile->name, _profile->measCount, _profile->measCount == 1 ? "" : "s",
             static_cast<unsigned long>(_profile->defaultIntervalMs));

    // One-time init commands (range/function/speed...).
    for (uint8_t i = 0; i < _profile->initCount; ++i)
    {
        ESP_LOGI(LOG_TAG, "init: %s", _profile->init[i]);
        _tmc.writeCommand(_profile->init[i]);
    }

    _activeMeas = 0;
    _intervalMs = _profile->defaultIntervalMs;
    // The profile decides whether logging starts right away (headless
    // campaigns, power-loss recovery) or waits for OK / the web (clean data
    // for hand-operated instruments). Every connect starts in this state.
    _running = _profile->autostart;
    _deviceReady = true;
    if (_display)
    {
        // Show the profile's `name` (like the web page does); yellow marks a
        // profile loaded from the SD card as opposed to a firmware built-in.
        bool fromSd = (_profile == &_profileBuf);
        _display->setStatus(_profile->name, fromSd);
        if (!_running)
            _display->setValue(literals::ui_paused); // OK starts the log
    }
}

// --- Connect / disconnect --------------------------------------------------

void InstrumentTask::handleConnect(uint8_t address)
{
    ESP_LOGI(LOG_TAG, "Device connected at address %u", address);

    esp_err_t err = usb_host_device_open(_client, address, &_session.dev);
    if (err != ESP_OK)
    {
        ESP_LOGE(LOG_TAG, "usb_host_device_open failed: %s", esp_err_to_name(err));
        return;
    }
    _session.address = address;

    const usb_device_desc_t *desc = nullptr;
    if (usb_host_get_device_descriptor(_session.dev, &desc) == ESP_OK && desc)
    {
        _session.vid = desc->idVendor;
        _session.pid = desc->idProduct;
        ESP_LOGI(LOG_TAG, "VID:PID = 0x%04X:0x%04X", _session.vid, _session.pid);
    }

    char line[64];
    std::snprintf(line, sizeof(line), literals::fmt_usb_vidpid, _session.vid, _session.pid);

    if (!_tmc.open(_client, _session.dev))
    {
        if (_display)
        {
            _display->setStatus(literals::ui_not_usbtmc);
            _display->setValue(line);
        }
        return;
    }

    if (_display)
    {
        _display->setStatus(literals::ui_reading_idn);
        _display->setValue(line);
    }

    vTaskDelay(pdMS_TO_TICKS(POST_ENUM_SETTLE_MS));

    char idn[128];
    size_t idnLen = 0;
    bool ok = false;
    for (int attempt = 1; attempt <= IDN_RETRIES && !ok; ++attempt)
    {
        ok = _tmc.query("*IDN?", idn, sizeof(idn), &idnLen);
        if (!ok)
        {
            ESP_LOGW(LOG_TAG, "*IDN? attempt %d/%d failed", attempt, IDN_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(IDN_RETRY_MS));
        }
    }

    if (!ok)
    {
        // The instrument may be wedged mid-transfer from a previous session
        // (kit reboot, aborted read). A VBUS power cycle cannot fix that on
        // self-powered instruments (DP832A runs off the mains), so first try
        // the USBTMC-spec recovery: INITIATE_CLEAR aborts the stuck transfer.
        if (_tmc.deviceClear())
        {
            for (int attempt = 1; attempt <= 2 && !ok; ++attempt)
            {
                ok = _tmc.query("*IDN?", idn, sizeof(idn), &idnLen);
                if (!ok)
                    vTaskDelay(pdMS_TO_TICKS(IDN_RETRY_MS));
            }
            ESP_LOGI(LOG_TAG, "*IDN? after device clear: %s", ok ? "OK" : "still failing");
        }
    }

    if (!ok)
    {
        // The DM3058E sometimes comes up with a dead USBTMC pipe on the first
        // enumeration and does NOT re-enumerate on its own. Force it: close the
        // handle and power-cycle host VBUS so the device re-attaches cleanly.
        if (_reenumCount < MAX_REENUM)
        {
            _reenumCount++;
            ESP_LOGW(LOG_TAG, "*IDN? failed - forcing re-enumeration %d/%d",
                     _reenumCount, MAX_REENUM);
            if (_display)
                _display->setStatus(literals::ui_redetecting);
            forceReenumerate();
        }
        else
        {
            ESP_LOGE(LOG_TAG, "*IDN? failed - giving up, replug the instrument");
            _reenumCount = 0;
            closeSession();
            if (_display)
            {
                _display->setStatus(literals::ui_idn_failed);
                _display->setValue("");
            }
        }
        return;
    }

    ESP_LOGI(LOG_TAG, "IDN = %s", idn);
    _reenumCount = 0;
    std::strncpy(_idn, idn, sizeof(_idn) - 1);
    _idn[sizeof(_idn) - 1] = '\0';

    selectProfile(idn);
    publishUi();
    syncMeasuring();
}

void InstrumentTask::closeSession()
{
    _tmc.close();
    if (_session.dev)
        usb_host_device_close(_client, _session.dev);
    _session = InstrumentSession{};
}

void InstrumentTask::forceReenumerate()
{
    // Drop our handle first, then briefly cut VBUS. The device disconnects
    // (DEV_GONE) and re-attaches (NEW_DEV -> handleConnect) with a fresh pipe.
    closeSession();
    bsp_usb_host_power_mode(BSP_USB_HOST_POWER_MODE_OFF, HW_USB_HOST_LIMIT_500MA);
    vTaskDelay(pdMS_TO_TICKS(300));
    bsp_usb_host_power_mode(HW_USB_HOST_POWER_MODE, HW_USB_HOST_LIMIT_500MA);
}

void InstrumentTask::handleDisconnect()
{
    ESP_LOGW(LOG_TAG, "Device gone");
    _deviceReady = false;
    endMeasuring();
    closeSession();
    _profile = nullptr;
    _idn[0] = '\0';
    publishUi();

    if (_display)
    {
        // Center stage, like the PC-mode prompt - it is the main message
        // when nothing is attached.
        _display->setStatus("");
        _display->setValue(literals::ui_waiting);
    }
}

// --- Measurement session ---------------------------------------------------

void InstrumentTask::beginMeasuring()
{
    if (_measuring || !_profile || _activeMeas >= _profile->measCount)
        return;

    // select_single: one column (the active DMM function). Multi-channel
    // (DP832A): every profile measurement becomes a CSV column.
    const char *cols[Profile::MAX_MEAS];
    uint8_t colCount = 0;
    if (_profile->selectSingle)
    {
        cols[colCount++] = _profile->meas[_activeMeas].column;
    }
    else
    {
        for (uint8_t i = 0; i < _profile->measCount; ++i)
            cols[colCount++] = _profile->meas[i].column;
    }

    if (_storage)
        _storage->beginSession(_idn, _intervalMs, _profile->id, _profile->name,
                               cols, colCount);
    _measStartTick = xTaskGetTickCount();
    _nextMeasTick = _measStartTick;
    _measuring = true;
    ESP_LOGI(LOG_TAG, "measuring %s (%u column%s) every %lu ms",
             _profile->selectSingle ? _profile->meas[_activeMeas].label : _profile->name,
             colCount, colCount == 1 ? "" : "s",
             static_cast<unsigned long>(_intervalMs));
}

void InstrumentTask::endMeasuring()
{
    if (!_measuring)
        return;
    if (_storage)
        _storage->endSession();
    _measuring = false;
}

void InstrumentTask::syncMeasuring()
{
    bool want = _deviceReady && _running && _profile;
    if (want && !_measuring)
        beginMeasuring();
    else if (!want && _measuring)
        endMeasuring();
}

void InstrumentTask::restartSession()
{
    if (_measuring)
    {
        endMeasuring();
        beginMeasuring();
    }
}

// --- Commands from the menu ------------------------------------------------

void InstrumentTask::processCommands()
{
    Cmd c;
    bool changed = false;
    while (_cmdQueue && xQueueReceive(_cmdQueue, &c, 0) == pdTRUE)
    {
        switch (c.type)
        {
        case Cmd::Type::CycleInterval:
        {
            size_t idx = meas::intervalIndexFor(_intervalMs);
            idx = (idx + 1) % meas::INTERVAL_COUNT;
            _intervalMs = meas::INTERVALS_MS[idx];
            _nextMeasTick = xTaskGetTickCount();
            ESP_LOGI(LOG_TAG, "interval -> %lu ms", static_cast<unsigned long>(_intervalMs));
            changed = true;
            break;
        }
        case Cmd::Type::CycleQuantity:
            if (_profile && _profile->measCount > 1)
            {
                _activeMeas = (_activeMeas + 1) % _profile->measCount;
                ESP_LOGI(LOG_TAG, "quantity -> %s", _profile->meas[_activeMeas].label);
                // select_single: the logged column changes -> new CSV file.
                // Multi-channel: all columns are always logged, the active
                // measurement only picks what the Live screen shows.
                if (_profile->selectSingle)
                    restartSession();
                changed = true;
            }
            break;
        case Cmd::Type::ToggleRunning:
        case Cmd::Type::SetRunning:
        {
            if (!_deviceReady)
                break; // nothing to start/stop without an instrument
            bool want = (c.type == Cmd::Type::ToggleRunning) ? !_running : c.arg;
            if (want != _running)
            {
                _running = want;
                syncMeasuring();
                // Immediate feedback on the Live screen.
                if (!_running && _display)
                    _display->setValue(literals::ui_paused);
                changed = true;
            }
            break;
        }
        case Cmd::Type::RestartSession:
            // After an SD re-mount: reopen the CSV session so rows flow into a
            // fresh file again (no-op unless currently measuring).
            restartSession();
            break;
        }
    }
    if (changed)
        publishUi();
}

void InstrumentTask::doMeasurement(double relSeconds)
{
    if (!_profile || _activeMeas >= _profile->measCount)
        return;

    // select_single: query just the active function. Multi-channel: query all
    // measurements back-to-back so one poll cycle = one CSV row. A failed
    // query becomes NAN in its column instead of losing the whole row.
    const uint8_t first = _profile->selectSingle ? _activeMeas : 0;
    const uint8_t count = _profile->selectSingle ? 1 : _profile->measCount;

    double values[Profile::MAX_MEAS];
    bool anyOk = false;
    for (uint8_t i = 0; i < count; ++i)
    {
        const Measurement &m = _profile->meas[first + i];
        char resp[64];
        if (_tmc.query(m.scpi, resp, sizeof(resp), nullptr))
        {
            values[i] = std::atof(resp);
            anyOk = true;
        }
        else
        {
            ESP_LOGW(LOG_TAG, "read failed: %s", m.label);
            values[i] = NAN;
        }
    }
    if (!anyOk)
        return; // all reads failed - no row, no display update

    // Snapshot for the web /api/status.
    if (_uiMutex && xSemaphoreTake(_uiMutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        _live = LiveValues{};
        _live.count = count;
        _live.relSeconds = relSeconds;
        for (uint8_t i = 0; i < count; ++i)
        {
            const Measurement &m = _profile->meas[first + i];
            std::strncpy(_live.label[i], m.label, sizeof(_live.label[i]) - 1);
            std::strncpy(_live.unit[i], m.unit, sizeof(_live.unit[i]) - 1);
            _live.value[i] = values[i];
        }
        xSemaphoreGive(_uiMutex);
    }

    const uint8_t shown = _profile->selectSingle ? 0 : _activeMeas;
    const Measurement &dm = _profile->meas[_activeMeas];
    ESP_LOGI(LOG_TAG, "%8.3f s  %s = %.6f %s", relSeconds, dm.label, values[shown], dm.unit);

    if (_display)
    {
        // select_single: the one reading, big. Multi-channel: all channels
        // under each other (smaller font on the display side).
        char l[224];
        if (_profile->selectSingle)
        {
            std::snprintf(l, sizeof(l), literals::fmt_value, values[0], dm.unit);
        }
        else
        {
            uint8_t lines = count > MAX_LIVE_LINES ? MAX_LIVE_LINES : count;
            int pos = 0;
            for (uint8_t i = 0; i < lines && pos < static_cast<int>(sizeof(l)); ++i)
            {
                const Measurement &m = _profile->meas[i];
                pos += std::snprintf(l + pos, sizeof(l) - pos,
                                     literals::fmt_value_line,
                                     i ? "\n" : "", m.label, values[i], m.unit);
            }
        }
        _display->setValue(l);
    }
    if (_storage)
        _storage->pushSample(SOURCE_ID, relSeconds, values, count);
}

// --- Task loop -------------------------------------------------------------

void InstrumentTask::loop()
{
    _display = Application::getInstance()->getDisplayTask();
    _storage = Application::getInstance()->getStorageTask();
    publishUi();

    bool ok = startHost();
    Application::getInstance()->signalTaskStart(Application::TaskBit::Instrument);

    if (!ok)
    {
        if (_display)
            _display->setStatus(literals::ui_usb_host_failed);
        while (true)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (_display)
    {
        _display->setStatus("");
        _display->setValue(literals::ui_waiting);
    }

    while (true)
    {
        usb_host_client_handle_events(_client, pdMS_TO_TICKS(50));

        if (_pendingDisconnect)
        {
            _pendingDisconnect = false;
            handleDisconnect();
        }
        if (_pendingConnect)
        {
            _pendingConnect = false;
            handleConnect(_pendingAddress);
        }

        processCommands();

        if (_measuring)
        {
            TickType_t now = xTaskGetTickCount();
            if (static_cast<int32_t>(now - _nextMeasTick) >= 0)
            {
                double relSec = (now - _measStartTick) * portTICK_PERIOD_MS / 1000.0;
                _nextMeasTick += pdMS_TO_TICKS(_intervalMs);
                doMeasurement(relSec);
            }
        }
    }
}
