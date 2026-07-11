//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   storage_task.h
/// @author Petr Vanek
///
/// Consumer task: owns the SD card and the CSV writer. It receives typed
/// messages from the InstrumentTask over a queue 
///
/// Messages:
///   SessionStart - a new instrument started logging -> open a fresh CSV
///   Data         - one measured sample -> append a row
///   SessionStop  - the instrument went away -> close the file
///

#pragma once

#include "rptask.h"
#include "csv_logger.h"
#include "profile.h"

class StorageTask : public RPTask
{
public:
    /// SD status snapshot for the UI.
    struct SdInfo
    {
        bool mounted{false};
        uint64_t totalBytes{0};
        uint64_t freeBytes{0};
    };

    StorageTask();
    virtual ~StorageTask();

    /// Open a new logging session for an instrument identified by @p idn.
    /// @p fileAlias (profile id) names the CSV file, @p instrumentName goes to
    /// the header metadata, @p columns lists the value column names (order =
    /// order of values in pushSample). Non-blocking.
    bool beginSession(const char *idn, uint32_t intervalMs,
                      const char *fileAlias, const char *instrumentName,
                      const char *const *columns, uint8_t columnCount);

    /// Queue one row: relative time [s] plus @p count values (one per session
    /// column), tagged by source id (foundation for multi-instrument, M8).
    /// Non-blocking; drops if the queue is full rather than stalling the producer.
    bool pushSample(uint8_t sourceId, double relSeconds,
                    const double *values, uint8_t count);

    /// Close the current session's file. Non-blocking.
    void endSession();

    /// Safe-remove: close the CSV file and unmount the card so it can be
    /// pulled out (or the power switched off) without corrupting the FAT.
    /// Non-blocking; sdInfo().mounted goes false once done.
    void requestEject();

    /// Re-mount a (re-)inserted card. Non-blocking. The producer must restart
    /// its session afterwards (InstrumentTask::restartLogging()).
    void requestMount();

    /// Whether the SD card mounted at startup (for the UI / menu, C3b).
    bool isStorageReady() const { return _sdReady; }

    /// Read-only SD snapshot (mounted + free/total bytes). Safe from any task.
    /// NOTE: queries the filesystem - do not poll it (see PROGRESS: the free-
    /// cluster scan on a fresh card takes seconds); poll isStorageReady().
    SdInfo sdInfo() const;

    /// Basename of the CSV currently being written ("" when not logging).
    void currentFile(char *buf, size_t cap) const;

protected:
    void loop() override;

private:
    static constexpr const char *LOG_TAG = "STOR";

    static constexpr const char *LOG_DIR = "/sdcard/logs";

    static constexpr uint8_t MAX_COLUMNS = Profile::MAX_MEAS;
    static_assert(CsvLogger::MAX_VALUES >= Profile::MAX_MEAS,
                  "CsvLogger row capacity must cover a full profile");
    static constexpr size_t IDN_MAX = 96;
    static constexpr size_t ALIAS_MAX = 32;      ///< = sizeof(Profile::id)
    static constexpr size_t INSTR_NAME_MAX = 48; ///< = sizeof(Profile::name)
    static constexpr size_t COLUMN_MAX = 24;
    static constexpr UBaseType_t QUEUE_DEPTH = 16;

    struct Msg
    {
        enum class Type : uint8_t
        {
            SessionStart,
            Data,
            SessionStop,
            Eject,
            Mount,
        } type;
        uint8_t sourceId;
        uint8_t count; ///< columns (SessionStart) / values (Data)
        uint32_t intervalMs;
        double relSeconds;
        double values[MAX_COLUMNS];
        char idn[IDN_MAX];
        char alias[ALIAS_MAX];
        char name[INSTR_NAME_MAX];
        char columns[MAX_COLUMNS][COLUMN_MAX];
    };

    bool mountStorage();
    bool post(const Msg &msg, TickType_t wait);
    void announceFile(); ///< push the current CSV basename to the display

    QueueHandle_t _queue{nullptr};
    CsvLogger _logger;
    volatile bool _sdReady{false}; ///< changes at runtime now (eject/mount)
    char _lastFile[48]{};          ///< last basename sent to the display
};
