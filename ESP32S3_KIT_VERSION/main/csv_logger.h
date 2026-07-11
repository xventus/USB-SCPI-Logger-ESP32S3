//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   csv_logger.h
/// @author Petr Vanek
///
/// Writes measurement samples to a CSV file on the SD card.
///
/// Time is RELATIVE to the start of logging (no RTC yet ;-) ), so the
/// header records `time_source: uptime` and the timestamp column holds seconds
/// since the first sample of the session.
///

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>

class CsvLogger
{
public:
    CsvLogger() = default;
    ~CsvLogger();

    CsvLogger(const CsvLogger &) = delete;
    CsvLogger &operator=(const CsvLogger &) = delete;

    static constexpr size_t MAX_VALUES = 12; ///< value columns per row (= Profile::MAX_MEAS)

    /// Start a logging session under @p baseDir (created if missing). Metadata
    /// is remembered so rotated segments get the same header. @p columns is the
    /// already comma-joined list of value column names (M4b: one per profile
    /// measurement, e.g. "ch1_v,ch1_i"). Returns false on any filesystem error.
    bool open(const char *baseDir, const char *fileAlias,
              const char *metaInstrument, const char *idn,
              const char *metaProfile, uint32_t intervalMs,
              const char *columns);

    /// Append one row: relative time [s] plus @p count measured values (order
    /// matches the column list given to open()). Handles periodic f_sync and
    /// size-based rotation transparently.
    bool writeRow(double relSeconds, const double *values, size_t count);

    void close();

    bool isOpen() const { return _fp != nullptr; }
    const char *path() const { return _path; }

private:
    static constexpr char SEPARATOR = ',';            ///< CSV delimiter 
    static constexpr uint32_t SYNC_EVERY_ROWS = 5;    ///< f_sync after N rows...
    static constexpr int64_t SYNC_EVERY_US = 3000000; ///< ...or after 3 s 
    static constexpr size_t ROTATE_BYTES = 10 * 1024 * 1024; ///< new segment past 10 MB

    bool openSegment(); ///< probe a free name, open it, write the header
    void syncNow();

    FILE *_fp{nullptr};
    char _path[128]{};

    // Session metadata kept so a rotated segment can rewrite the header.
    char _dir[64]{};
    char _alias[40]{};
    char _instrument[48]{};
    char _idn[104]{};
    char _profile[40]{};
    char _columns[304]{}; ///< joined column names (MAX_VALUES x name + separators)
    uint32_t _intervalMs{0};

    int _segment{0};             ///< 1-based segment counter within the session
    size_t _bytesInSegment{0};   ///< bytes written to the current file
    uint32_t _rowsSinceSync{0};
    int64_t _lastSyncUs{0};
};
