//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   web_server.cpp
/// @author Petr Vanek

#include "web_server.h"
#include "application.h"
#include "instrument_task.h"
#include "storage_task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "WEB";

static constexpr const char *WEB_ROOT = "/web";
static constexpr const char *LOG_DIR = "/sdcard/logs";
static constexpr const char *PROFILE_DIR = "/sdcard/profiles";
static constexpr size_t CHUNK = 4096;
static constexpr size_t UPLOAD_MAX = 8192; ///< profile JSON size cap

static httpd_handle_t s_server = nullptr;

// --- helpers ----------------------------------------------------------------

/// File name from a wildcard URI ("/api/files/<name>"). Rejects anything that
/// could escape the directory. Returns false when invalid.
static bool uriFileName(const char *uri, const char *prefix, char *out, size_t cap)
{
    size_t plen = std::strlen(prefix);
    if (std::strncmp(uri, prefix, plen) != 0)
        return false;
    const char *name = uri + plen;
    if (name[0] == '\0' || std::strchr(name, '/') || std::strstr(name, ".."))
        return false;
    if (std::strlen(name) >= cap)
        return false;
    std::strcpy(out, name);
    return true;
}

/// Stream a file as an attachment. Returns ESP_OK even on 404 (response sent).
static esp_err_t sendFile(httpd_req_t *req, const char *dir, const char *name,
                          const char *contentType)
{
    char path[300];
    std::snprintf(path, sizeof(path), "%s/%s", dir, name);

    FILE *f = std::fopen(path, "rb");
    if (!f)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
        return ESP_OK;
    }

    httpd_resp_set_type(req, contentType);
    char disp[160];
    std::snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char *buf = static_cast<char *>(malloc(CHUNK));
    if (!buf)
    {
        std::fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = std::fread(buf, 1, CHUNK, f)) > 0)
    {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK)
        {
            err = ESP_FAIL; // client gone
            break;
        }
    }
    free(buf);
    std::fclose(f);
    if (err == ESP_OK)
        httpd_resp_send_chunk(req, nullptr, 0);
    return err;
}

/// List a directory as JSON: [{"name":...,"size":...},...]
static esp_err_t sendDirList(httpd_req_t *req, const char *dir)
{
    httpd_resp_set_type(req, "application/json");

    DIR *d = opendir(dir);
    if (!d)
    {
        httpd_resp_sendstr(req, "[]"); // no dir yet = no files
        return ESP_OK;
    }

    httpd_resp_send_chunk(req, "[", 1);
    bool first = true;
    struct dirent *ent;
    char item[340];
    while ((ent = readdir(d)) != nullptr)
    {
        if (ent->d_name[0] == '.')
            continue;
        char path[300];
        std::snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        long size = (stat(path, &st) == 0) ? static_cast<long>(st.st_size) : 0;

        int n = std::snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"size\":%ld}",
                              first ? "" : ",", ent->d_name, size);
        first = false;
        if (n > 0)
            httpd_resp_send_chunk(req, item, n);
    }
    closedir(d);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// --- handlers ---------------------------------------------------------------

static esp_err_t hRoot(httpd_req_t *req)
{
    char path[64];
    std::snprintf(path, sizeof(path), "%s/index.html", WEB_ROOT);
    FILE *f = std::fopen(path, "rb");
    if (!f)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "web page not flashed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    char *buf = static_cast<char *>(malloc(CHUNK));
    if (!buf)
    {
        std::fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    size_t n;
    while ((n = std::fread(buf, 1, CHUNK, f)) > 0)
    {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK)
            break;
    }
    free(buf);
    std::fclose(f);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t hStatus(httpd_req_t *req)
{
    auto *app = Application::getInstance();

    InstrumentTask::UiState ui{};
    app->getInstrumentTask()->getUiState(ui);
    InstrumentTask::LiveValues live{};
    app->getInstrumentTask()->getLiveValues(live);

    // sdInfo() hits the filesystem (free-cluster scan; see PROGRESS) and the
    // page polls once a second - cache it and refresh only occasionally.
    static StorageTask::SdInfo info;
    static int64_t infoAtUs = -1;
    int64_t now = esp_timer_get_time();
    if (infoAtUs < 0 || now - infoAtUs > 10 * 1000000LL)
    {
        info = app->getStorageTask()->sdInfo();
        infoAtUs = now;
    }

    char file[48];
    app->getStorageTask()->currentFile(file, sizeof(file));

    // Values: NaN (failed reading) must become null - JSON has no NaN.
    char values[Profile::MAX_MEAS * 64];
    size_t pos = 0;
    for (uint8_t i = 0; i < live.count && pos < sizeof(values) - 64; ++i)
    {
        if (std::isnan(live.value[i]))
            pos += std::snprintf(values + pos, sizeof(values) - pos,
                                 "%s{\"label\":\"%s\",\"value\":null,\"unit\":\"%s\"}",
                                 i ? "," : "", live.label[i], live.unit[i]);
        else
            pos += std::snprintf(values + pos, sizeof(values) - pos,
                                 "%s{\"label\":\"%s\",\"value\":%.6f,\"unit\":\"%s\"}",
                                 i ? "," : "", live.label[i], live.value[i], live.unit[i]);
    }
    values[pos] = '\0';

    char body[Profile::MAX_MEAS * 64 + 400];
    std::snprintf(body, sizeof(body),
                  "{\"idn\":\"%s\",\"profile\":\"%s\",\"running\":%s,"
                  "\"intervalMs\":%lu,\"file\":\"%s\","
                  "\"sd\":{\"mounted\":%s,\"freeMb\":%llu,\"totalMb\":%llu},"
                  "\"values\":[%s]}",
                  ui.idn, ui.profileName, ui.running ? "true" : "false",
                  static_cast<unsigned long>(ui.intervalMs), file,
                  info.mounted ? "true" : "false",
                  info.freeBytes / (1024ULL * 1024ULL),
                  info.totalBytes / (1024ULL * 1024ULL),
                  values);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

static esp_err_t hFilesList(httpd_req_t *req) { return sendDirList(req, LOG_DIR); }
static esp_err_t hProfilesList(httpd_req_t *req) { return sendDirList(req, PROFILE_DIR); }

static esp_err_t hFileGet(httpd_req_t *req)
{
    char name[64];
    if (!uriFileName(req->uri, "/api/files/", name, sizeof(name)))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_OK;
    }
    return sendFile(req, LOG_DIR, name, "text/csv");
}

static esp_err_t hProfileGet(httpd_req_t *req)
{
    char name[64];
    if (!uriFileName(req->uri, "/api/profiles/", name, sizeof(name)))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_OK;
    }
    return sendFile(req, PROFILE_DIR, name, "application/json");
}

static esp_err_t hProfilePut(httpd_req_t *req)
{
    char name[64];
    if (!uriFileName(req->uri, "/api/profiles/", name, sizeof(name)))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_OK;
    }
    if (req->content_len == 0 || req->content_len > UPLOAD_MAX)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_OK;
    }
    if (!Application::getInstance()->getStorageTask()->isStorageReady())
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
        return ESP_OK;
    }

    char *buf = static_cast<char *>(malloc(req->content_len));
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    size_t got = 0;
    while (got < req->content_len)
    {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n <= 0)
        {
            free(buf);
            return ESP_FAIL;
        }
        got += static_cast<size_t>(n);
    }

    mkdir(PROFILE_DIR, 0777); // ignore EEXIST

    char path[300];
    std::snprintf(path, sizeof(path), "%s/%s", PROFILE_DIR, name);
    FILE *f = std::fopen(path, "w");
    bool ok = f && std::fwrite(buf, 1, got, f) == got;
    if (f)
        std::fclose(f);
    free(buf);

    if (!ok)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "profile uploaded: %s (%u B)", name, static_cast<unsigned>(got));
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/// Shared DELETE handler: remove a file from @p dir (name from the URI).
static esp_err_t deleteFrom(httpd_req_t *req, const char *prefix, const char *dir,
                            bool guardActiveCsv)
{
    char name[64];
    if (!uriFileName(req->uri, prefix, name, sizeof(name)))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_OK;
    }

    if (guardActiveCsv)
    {
        // Never delete the CSV that is being written right now.
        char active[48];
        Application::getInstance()->getStorageTask()->currentFile(active, sizeof(active));
        if (active[0] != '\0' && std::strcmp(active, name) == 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "file is being written - stop logging first");
            return ESP_OK;
        }
    }

    char path[300];
    std::snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (std::remove(path) != 0)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "deleted %s", path);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t hFileDelete(httpd_req_t *req)
{
    return deleteFrom(req, "/api/files/", LOG_DIR, true);
}

static esp_err_t hProfileDelete(httpd_req_t *req)
{
    return deleteFrom(req, "/api/profiles/", PROFILE_DIR, false);
}

static esp_err_t hControl(httpd_req_t *req)
{
    char body[128];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0)
        return ESP_FAIL;
    body[len] = '\0';

    InstrumentTask::UiState ui{};
    Application::getInstance()->getInstrumentTask()->getUiState(ui);
    if (!ui.deviceReady)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no instrument connected");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    const cJSON *action = root ? cJSON_GetObjectItemCaseSensitive(root, "action") : nullptr;
    bool ok = false;
    if (cJSON_IsString(action))
    {
        if (std::strcmp(action->valuestring, "start") == 0)
        {
            Application::getInstance()->getInstrumentTask()->setRunning(true);
            ok = true;
        }
        else if (std::strcmp(action->valuestring, "stop") == 0)
        {
            Application::getInstance()->getInstrumentTask()->setRunning(false);
            ok = true;
        }
    }
    cJSON_Delete(root);

    if (!ok)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad action");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// --- start / stop -----------------------------------------------------------

bool webServerStart()
{
    if (s_server)
        return true;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_server, &cfg) != ESP_OK)
    {
        s_server = nullptr;
        return false;
    }

    static const httpd_uri_t routes[] = {
        {"/api/status", HTTP_GET, hStatus, nullptr},
        {"/api/files/*", HTTP_GET, hFileGet, nullptr},
        {"/api/files/*", HTTP_DELETE, hFileDelete, nullptr},
        {"/api/files", HTTP_GET, hFilesList, nullptr},
        {"/api/profiles/*", HTTP_PUT, hProfilePut, nullptr},
        {"/api/profiles/*", HTTP_GET, hProfileGet, nullptr},
        {"/api/profiles/*", HTTP_DELETE, hProfileDelete, nullptr},
        {"/api/profiles", HTTP_GET, hProfilesList, nullptr},
        {"/api/control", HTTP_POST, hControl, nullptr},
        {"/", HTTP_GET, hRoot, nullptr},
        {"/index.html", HTTP_GET, hRoot, nullptr},
    };
    for (const auto &r : routes)
        httpd_register_uri_handler(s_server, &r);

    ESP_LOGI(TAG, "web server up");
    return true;
}

void webServerStop()
{
    if (s_server)
    {
        httpd_stop(s_server);
        s_server = nullptr;
        ESP_LOGI(TAG, "web server stopped");
    }
}
