//
// vim: ts=4 et
// Copyright (c) 2025 Petr Vanek, petr@fotoventus.cz
//
/// @file   storage_task.cpp
/// @author Petr Vanek

#include "storage_task.h"
#include "application.h"
#include "display_task.h"
#include "bsp/esp32_s3_usb_otg.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include <cstring>
#include <cstdio>

StorageTask::StorageTask()
{
    _queue = xQueueCreate(QUEUE_DEPTH, sizeof(Msg));
}

StorageTask::~StorageTask()
{
    done();
    if (_queue)
    {
        vQueueDelete(_queue);
    }
}

bool StorageTask::mountStorage()
{
    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK)
    {
        ESP_LOGW(LOG_TAG, "SD mount failed: %s (logging disabled)", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(LOG_TAG, "SD card mounted at %s", BSP_SD_MOUNT_POINT);
    return true;
}

bool StorageTask::post(const Msg &msg, TickType_t wait)
{
    if (!_queue)
    {
        return false;
    }
    return xQueueSend(_queue, &msg, wait) == pdTRUE;
}

void StorageTask::currentFile(char *buf, size_t cap) const
{
    if (!buf || cap == 0)
        return;
    std::strncpy(buf, _lastFile, cap - 1);
    buf[cap - 1] = '\0';
}

void StorageTask::announceFile()
{
    // Basename of the open CSV ("" when not logging). Also catches segment
    // changes from size-based rotation inside writeRow().
    const char *base = "";
    if (_logger.isOpen())
    {
        const char *p = _logger.path();
        const char *slash = std::strrchr(p, '/');
        base = slash ? slash + 1 : p;
    }
    if (std::strcmp(base, _lastFile) != 0)
    {
        std::strncpy(_lastFile, base, sizeof(_lastFile) - 1);
        _lastFile[sizeof(_lastFile) - 1] = '\0';
        Application::getInstance()->getDisplayTask()->setFile(_lastFile);
    }
}

bool StorageTask::beginSession(const char *idn, uint32_t intervalMs,
                               const char *fileAlias, const char *instrumentName,
                               const char *const *columns, uint8_t columnCount)
{
    Msg msg{};
    msg.type = Msg::Type::SessionStart;
    msg.intervalMs = intervalMs;
    std::strncpy(msg.idn, idn ? idn : "", IDN_MAX - 1);
    std::strncpy(msg.alias, (fileAlias && fileAlias[0]) ? fileAlias : "instrument", ALIAS_MAX - 1);
    std::strncpy(msg.name, instrumentName ? instrumentName : "", INSTR_NAME_MAX - 1);

    if (columnCount > MAX_COLUMNS)
    {
        columnCount = MAX_COLUMNS;
    }
    msg.count = (columnCount > 0) ? columnCount : 1;
    for (uint8_t i = 0; i < msg.count; ++i)
    {
        const char *col = (columns && columns[i]) ? columns[i] : "value";
        std::strncpy(msg.columns[i], col, COLUMN_MAX - 1);
    }
    return post(msg, pdMS_TO_TICKS(50));
}

StorageTask::SdInfo StorageTask::sdInfo() const
{
    SdInfo info;
    info.mounted = _sdReady;
    if (_sdReady)
    {
        uint64_t total = 0, freeBytes = 0;
        if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &freeBytes) == ESP_OK)
        {
            info.totalBytes = total;
            info.freeBytes = freeBytes;
        }
    }
    return info;
}

bool StorageTask::pushSample(uint8_t sourceId, double relSeconds,
                             const double *values, uint8_t count)
{
    if (!values || count == 0)
    {
        return false;
    }
    if (count > MAX_COLUMNS)
    {
        count = MAX_COLUMNS;
    }
    Msg msg{};
    msg.type = Msg::Type::Data;
    msg.sourceId = sourceId;
    msg.relSeconds = relSeconds;
    msg.count = count;
    std::memcpy(msg.values, values, count * sizeof(double));
    // Drop rather than block the producer if the writer falls behind.
    return post(msg, 0);
}

void StorageTask::endSession()
{
    Msg msg{};
    msg.type = Msg::Type::SessionStop;
    post(msg, pdMS_TO_TICKS(50));
}

void StorageTask::requestEject()
{
    Msg msg{};
    msg.type = Msg::Type::Eject;
    post(msg, pdMS_TO_TICKS(50));
}

void StorageTask::requestMount()
{
    Msg msg{};
    msg.type = Msg::Type::Mount;
    post(msg, pdMS_TO_TICKS(50));
}

void StorageTask::loop()
{
    _sdReady = mountStorage();
    Application::getInstance()->signalTaskStart(Application::TaskBit::Storage);

    Msg msg{};
    while (true)
    {
        if (xQueueReceive(_queue, &msg, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        switch (msg.type)
        {
        case Msg::Type::SessionStart:
            if (_sdReady)
            {
                // Join the column names for the CSV header line.
                char joined[MAX_COLUMNS * COLUMN_MAX];
                size_t pos = 0;
                for (uint8_t i = 0; i < msg.count; ++i)
                {
                    int n = std::snprintf(joined + pos, sizeof(joined) - pos, "%s%s",
                                          i ? "," : "", msg.columns[i]);
                    if (n <= 0)
                        break;
                    pos += static_cast<size_t>(n);
                }
                // Profile id doubles as the file alias and the `# profile:` meta.
                _logger.open(LOG_DIR, msg.alias, msg.name, msg.idn,
                             msg.alias, msg.intervalMs, joined);
            }
            else
            {
                ESP_LOGW(LOG_TAG, "no SD - session not logged");
            }
            announceFile();
            break;

        case Msg::Type::Data:
            if (_logger.isOpen())
            {
                if (!_logger.writeRow(msg.relSeconds, msg.values, msg.count))
                {
                    ESP_LOGW(LOG_TAG, "write failed, closing file");
                    _logger.close();
                }
                announceFile(); // rotation opens the next segment silently
            }
            break;

        case Msg::Type::SessionStop:
            _logger.close();
            announceFile();
            break;

        case Msg::Type::Eject:
            // Close (fsync) the CSV first, then unmount - after this the card
            // is clean and can be removed or the power cut safely.
            _logger.close();
            if (_sdReady)
            {
                bsp_sdcard_unmount();
                _sdReady = false;
                ESP_LOGI(LOG_TAG, "SD ejected - safe to remove");
            }
            announceFile();
            Application::getInstance()->getDisplayTask()->refreshMenu();
            break;

        case Msg::Type::Mount:
            if (!_sdReady)
            {
                _sdReady = mountStorage();
            }
            Application::getInstance()->getDisplayTask()->refreshMenu();
            break;
        }
    }
}
