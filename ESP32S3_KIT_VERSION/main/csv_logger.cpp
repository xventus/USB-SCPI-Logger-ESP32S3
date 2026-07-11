//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   csv_logger.cpp
/// @author Petr Vanek

#include "csv_logger.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

static const char *TAG = "CSV";

CsvLogger::~CsvLogger()
{
    close();
}

bool CsvLogger::open(const char *baseDir, const char *fileAlias,
                     const char *metaInstrument, const char *idn,
                     const char *metaProfile, uint32_t intervalMs,
                     const char *columns)
{
    close();

    // Remember everything the header needs so rotated segments can rewrite it.
    std::strncpy(_dir, baseDir, sizeof(_dir) - 1);
    std::strncpy(_alias, fileAlias, sizeof(_alias) - 1);
    std::strncpy(_instrument, metaInstrument, sizeof(_instrument) - 1);
    std::strncpy(_idn, idn ? idn : "", sizeof(_idn) - 1);
    std::strncpy(_profile, metaProfile, sizeof(_profile) - 1);
    std::strncpy(_columns, (columns && columns[0]) ? columns : "value", sizeof(_columns) - 1);
    _intervalMs = intervalMs;
    _segment = 0;

    return openSegment();
}

bool CsvLogger::openSegment()
{
    // Make sure the target directory exists (ignore "already exists").
    if (mkdir(_dir, 0777) != 0 && errno != EEXIST)
    {
        ESP_LOGE(TAG, "mkdir(%s) failed: %s", _dir, strerror(errno));
        return false;
    }

    // Pick the first free "<dir>/<alias>_NNNN.csv". After a rotation the
    // previous segment already exists, so this naturally advances the index.
    for (int idx = 1; idx <= 9999; ++idx)
    {
        std::snprintf(_path, sizeof(_path), "%s/%s_%04d.csv", _dir, _alias, idx);
        struct stat st;
        if (stat(_path, &st) != 0)
        {
            break; // does not exist -> use it
        }
        if (idx == 9999)  // TODO: some other action?
        {
            ESP_LOGE(TAG, "no free file name in %s", _dir);
            return false;
        }
    }

    _fp = std::fopen(_path, "w");
    if (!_fp)
    {
        ESP_LOGE(TAG, "fopen(%s) failed: %s", _path, strerror(errno));
        return false;
    }

    // Metadata comment block + column header 
    int n = 0;
    n += std::fprintf(_fp, "# instrument: %s\n", _instrument);
    n += std::fprintf(_fp, "# idn: %s\n", _idn);
    n += std::fprintf(_fp, "# profile: %s\n", _profile);
    n += std::fprintf(_fp, "# time_source: uptime\n");
    n += std::fprintf(_fp, "# interval_ms: %lu\n", static_cast<unsigned long>(_intervalMs));
    n += std::fprintf(_fp, "# segment: %d\n", _segment + 1);
    n += std::fprintf(_fp, "timestamp_s%c%s\n", SEPARATOR, _columns);

    _bytesInSegment = (n > 0) ? static_cast<size_t>(n) : 0;
    _segment++;
    syncNow();

    ESP_LOGI(TAG, "logging to %s (segment %d)", _path, _segment);
    return true;
}

void CsvLogger::syncNow()
{
    if (_fp)
    {
        std::fflush(_fp);
        fsync(fileno(_fp));
    }
    _rowsSinceSync = 0;
    _lastSyncUs = esp_timer_get_time();
}

bool CsvLogger::writeRow(double relSeconds, const double *values, size_t count)
{
    if (!_fp || !values || count == 0)
    {
        return false;
    }
    if (count > MAX_VALUES)
    {
        count = MAX_VALUES;
    }

    // Build the whole row first so a failed write is detected in one place.
    // A missing reading arrives as NAN and prints as "nan" (pandas-friendly).
    // Sized for MAX_VALUES worst-case %.6f (overload readings like 9.9e37
    // print ~45 chars each).
    char row[640];
    int len = std::snprintf(row, sizeof(row), "%.3f", relSeconds);
    for (size_t i = 0; i < count && len > 0 && len < static_cast<int>(sizeof(row)); ++i)
    {
        len += std::snprintf(row + len, sizeof(row) - len, "%c%.6f", SEPARATOR, values[i]);
    }
    if (len <= 0 || len >= static_cast<int>(sizeof(row)) - 1)
    {
        ESP_LOGE(TAG, "row too long (%d)", len);
        return false;
    }
    row[len++] = '\n';

    if (std::fwrite(row, 1, static_cast<size_t>(len), _fp) != static_cast<size_t>(len))
    {
        ESP_LOGE(TAG, "write failed: %s", strerror(errno));
        return false;
    }
    _bytesInSegment += static_cast<size_t>(len);
    _rowsSinceSync++;

    // f_sync after N rows or T seconds, whichever comes first.
    if (_rowsSinceSync >= SYNC_EVERY_ROWS ||
        (esp_timer_get_time() - _lastSyncUs) >= SYNC_EVERY_US)
    {
        syncNow();
    }

    // Size-based rotation: close this file and continue in the next segment.
    if (_bytesInSegment >= ROTATE_BYTES)
    {
        ESP_LOGI(TAG, "rotating (%u bytes) -> new segment", static_cast<unsigned>(_bytesInSegment));
        std::fflush(_fp);
        fsync(fileno(_fp));
        std::fclose(_fp);
        _fp = nullptr;
        if (!openSegment())
        {
            ESP_LOGE(TAG, "rotation failed, logging stopped");
            return false;
        }
    }
    return true;
}

void CsvLogger::close()
{
    if (_fp)
    {
        std::fflush(_fp);
        fsync(fileno(_fp));
        std::fclose(_fp);
        _fp = nullptr;
        ESP_LOGI(TAG, "closed %s", _path);
    }
}
