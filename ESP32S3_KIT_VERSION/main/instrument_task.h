//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   instrument_task.h
/// @author Petr Vanek
///
/// Owns the USB host role: powers VBUS, enumerates a USBTMC instrument, reads
/// *IDN?, selects a Profile (SD JSON or built-in) and polls the profile's
/// measurement(s), pushing samples to the StorageTask and the live value to
/// the DisplayTask.

#pragma once

#include "rptask.h"
#include "usb/usb_host.h"
#include "usbtmc.h"
#include "profile.h"
#include "measurement.h"
#include "freertos/semphr.h"

class DisplayTask;
class StorageTask;

class InstrumentTask : public RPTask
{
public:
    /// Snapshot the DisplayTask menu reads (no direct profile access).
    struct UiState
    {
        char profileName[48];
        char activeLabel[24];
        char activeUnit[12];
        char idn[96];
        uint32_t intervalMs;
        bool running;
        bool deviceReady;
        uint8_t measCount;
        uint8_t activeMeas;
        bool selectSingle;
    };

    /// Snapshot of the latest readings (web /api/status). count == 0 until
    /// the first measurement (or after a disconnect).
    struct LiveValues
    {
        uint8_t count;
        double relSeconds;
        char label[Profile::MAX_MEAS][24];
        char unit[Profile::MAX_MEAS][12];
        double value[Profile::MAX_MEAS];
    };

    InstrumentTask();
    virtual ~InstrumentTask();

    // --- Control API (called from the DisplayTask menu) ---
    void cycleInterval();   ///< next sampling interval
    void cycleQuantity();   ///< next measurement (DMM function / channel)
    void toggleRunning();   ///< start/stop logging
    void setRunning(bool run); ///< explicit start/stop (web /api/control)
    void restartLogging();  ///< reopen the CSV session (after SD re-mount)

    /// Copy the current UI snapshot (thread-safe).
    void getUiState(UiState &out) const;

    /// Copy the latest readings (thread-safe; web /api/status).
    void getLiveValues(LiveValues &out) const;

    /// Seconds since the current logging session started (0 = not logging).
    /// Matches the CSV relative timestamps (rotation does not reset it).
    uint32_t sessionSeconds() const;

protected:
    void loop() override;

private:
    static constexpr const char *LOG_TAG = "INST";
    static constexpr int IDN_RETRIES = 5;           ///< *IDN? attempts per enumeration
    static constexpr int IDN_RETRY_MS = 200;        ///< spacing between attempts
    static constexpr int POST_ENUM_SETTLE_MS = 250; ///< settle before first transfer
    static constexpr int MAX_REENUM = 2;            ///< VBUS power-cycles before giving up
    static constexpr uint8_t SOURCE_ID = 0;
    static constexpr uint8_t MAX_LIVE_LINES = 7; ///< value lines that fit the LCD
    static constexpr const char *PROFILE_DIR = "/sdcard/profiles";

    struct InstrumentSession
    {
        uint8_t address{0};
        usb_device_handle_t dev{nullptr};
        uint16_t vid{0};
        uint16_t pid{0};
    };

    struct Cmd
    {
        enum class Type : uint8_t
        {
            CycleInterval,
            CycleQuantity,
            ToggleRunning,
            SetRunning,
            RestartSession,
        } type;
        bool arg; ///< SetRunning
    };

    bool startHost();
    void handleConnect(uint8_t address);
    void handleDisconnect();
    void forceReenumerate(); ///< VBUS power-cycle to make the device re-attach
    void closeSession();     ///< close TMC + device handle, clear session
    void selectProfile(const char *idn);
    void doMeasurement(double relSeconds);
    void processCommands();

    void beginMeasuring();
    void endMeasuring();
    void syncMeasuring();
    void restartSession();
    void publishUi();

    static void clientEventCb(const usb_host_client_event_msg_t *event, void *arg);

    usb_host_client_handle_t _client{nullptr};
    InstrumentSession _session;
    UsbtmcDevice _tmc;
    DisplayTask *_display{nullptr};
    StorageTask *_storage{nullptr};
    QueueHandle_t _cmdQueue{nullptr};

    // Callback -> loop hand-off.
    volatile bool _pendingConnect{false};
    volatile uint8_t _pendingAddress{0};
    volatile bool _pendingDisconnect{false};
    int _reenumCount{0}; ///< forced re-enumerations in the current connect chain

    // Active profile + settings.
    const Profile *_profile{nullptr};
    Profile _profileBuf{}; ///< storage for an SD-loaded profile
    uint8_t _activeMeas{0};
    uint32_t _intervalMs{meas::INTERVALS_MS[meas::DEFAULT_INTERVAL_IDX]};
    bool _running{true};
    bool _deviceReady{false};
    char _idn[128]{};

    // Scheduling.
    bool _measuring{false};
    TickType_t _measStartTick{0};
    TickType_t _nextMeasTick{0};

    // UI snapshot for the menu + latest readings for the web (same mutex).
    mutable SemaphoreHandle_t _uiMutex{nullptr};
    UiState _ui{};
    LiveValues _live{};
};
