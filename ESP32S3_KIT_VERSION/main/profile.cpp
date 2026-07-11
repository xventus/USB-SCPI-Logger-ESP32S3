//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   profile.cpp
/// @author Petr Vanek

#include "profile.h"
#include "esp_log.h"
#include "cJSON.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>

static const char *TAG = "PROF";

// --- helpers ---------------------------------------------------------------

static void copyStr(char *dst, size_t cap, const char *src)
{
    if (!src)
    {
        dst[0] = '\0';
        return;
    }
    std::strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static void setMeas(Measurement &m, const char *scpi, const char *label,
                    const char *unit, const char *column)
{
    copyStr(m.scpi, sizeof(m.scpi), scpi);
    copyStr(m.label, sizeof(m.label), label);
    copyStr(m.unit, sizeof(m.unit), unit);
    copyStr(m.column, sizeof(m.column), column);
}

bool Profile::matches(const char *idn, uint16_t theVid) const
{
    if (vid != 0 && vid != theVid)
    {
        return false;
    }
    if (idnContains[0] != '\0')
    {
        if (!idn || std::strstr(idn, idnContains) == nullptr)
        {
            return false;
        }
    }
    return true;
}

// --- built-in profiles -----------------------------------------------------

static Profile g_builtins[2];
static bool g_builtinsReady = false;

static void ensureBuiltins()
{
    if (g_builtinsReady)
    {
        return;
    }

    // Rigol DM3058E - DMM, one function at a time (select_single).
    Profile &d = g_builtins[0];
    d = Profile{};
    copyStr(d.id, sizeof(d.id), "rigol_dm3058e");
    copyStr(d.name, sizeof(d.name), "Rigol DM3058E DMM");
    copyStr(d.idnContains, sizeof(d.idnContains), "DM3058");
    d.vid = 0x1AB1;
    d.selectSingle = true;
    d.autostart = true;
    d.defaultIntervalMs = 1000;
    d.measCount = 3;
    setMeas(d.meas[0], ":MEASure:VOLTage:DC?", "DC Voltage", "V", "voltage_dc");
    setMeas(d.meas[1], ":MEASure:CURRent:DC?", "DC Current", "A", "current_dc");
    setMeas(d.meas[2], ":MEASure:RESistance?", "Resistance", "Ohm", "resistance");
    d.initCount = 0;

    // Rigol DP832A - PSU, 3 channels logged simultaneously 
    Profile &p = g_builtins[1];
    p = Profile{};
    copyStr(p.id, sizeof(p.id), "rigol_dp832a");
    copyStr(p.name, sizeof(p.name), "Rigol DP832A PSU");
    copyStr(p.idnContains, sizeof(p.idnContains), "DP832");
    p.vid = 0x1AB1;
    p.selectSingle = false;
    p.autostart = true;
    p.defaultIntervalMs = 1000;
    p.measCount = 6;
    setMeas(p.meas[0], ":MEASure:VOLTage? CH1", "CH1 U", "V", "ch1_v");
    setMeas(p.meas[1], ":MEASure:CURRent? CH1", "CH1 I", "A", "ch1_i");
    setMeas(p.meas[2], ":MEASure:VOLTage? CH2", "CH2 U", "V", "ch2_v");
    setMeas(p.meas[3], ":MEASure:CURRent? CH2", "CH2 I", "A", "ch2_i");
    setMeas(p.meas[4], ":MEASure:VOLTage? CH3", "CH3 U", "V", "ch3_v");
    setMeas(p.meas[5], ":MEASure:CURRent? CH3", "CH3 I", "A", "ch3_i");
    p.initCount = 0;

    g_builtinsReady = true;
}

const Profile *profileMatchBuiltin(const char *idn, uint16_t vid)
{
    ensureBuiltins();
    for (const Profile &p : g_builtins)
    {
        if (p.matches(idn, vid))
        {
            return &p;
        }
    }
    return nullptr;
}

// --- JSON parsing ----------------------------------------------------------

bool profileParseJson(const char *json, Profile &out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
    {
        ESP_LOGW(TAG, "JSON parse error");
        return false;
    }

    out = Profile{};

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    copyStr(out.id, sizeof(out.id), cJSON_IsString(id) ? id->valuestring : "");
    copyStr(out.name, sizeof(out.name), cJSON_IsString(name) ? name->valuestring : out.id);

    const cJSON *match = cJSON_GetObjectItemCaseSensitive(root, "match");
    if (cJSON_IsObject(match))
    {
        const cJSON *idnc = cJSON_GetObjectItemCaseSensitive(match, "idn_contains");
        if (cJSON_IsString(idnc))
        {
            copyStr(out.idnContains, sizeof(out.idnContains), idnc->valuestring);
        }
        const cJSON *vid = cJSON_GetObjectItemCaseSensitive(match, "vid");
        if (cJSON_IsString(vid))
        {
            out.vid = static_cast<uint16_t>(std::strtol(vid->valuestring, nullptr, 0));
        }
        else if (cJSON_IsNumber(vid))
        {
            out.vid = static_cast<uint16_t>(vid->valueint);
        }
    }

    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(root, "default_interval_ms");
    out.defaultIntervalMs = cJSON_IsNumber(interval) ? static_cast<uint32_t>(interval->valueint) : 1000;

    const cJSON *sel = cJSON_GetObjectItemCaseSensitive(root, "select_single");
    out.selectSingle = cJSON_IsBool(sel) ? cJSON_IsTrue(sel) : false;

    // Missing = true: plug in and log (headless recovery after a power loss).
    const cJSON *autoStart = cJSON_GetObjectItemCaseSensitive(root, "autostart");
    out.autostart = cJSON_IsBool(autoStart) ? cJSON_IsTrue(autoStart) : true;

    // init[]
    const cJSON *init = cJSON_GetObjectItemCaseSensitive(root, "init");
    if (cJSON_IsArray(init))
    {
        const cJSON *cmd = nullptr;
        cJSON_ArrayForEach(cmd, init)
        {
            if (out.initCount >= Profile::MAX_INIT)
            {
                ESP_LOGW(TAG, "profile '%s': init[] truncated to %u", out.id, Profile::MAX_INIT);
                break;
            }
            if (cJSON_IsString(cmd))
            {
                copyStr(out.init[out.initCount], sizeof(out.init[0]), cmd->valuestring);
                out.initCount++;
            }
        }
    }

    // measurements[]
    const cJSON *meas = cJSON_GetObjectItemCaseSensitive(root, "measurements");
    if (cJSON_IsArray(meas))
    {
        const cJSON *m = nullptr;
        cJSON_ArrayForEach(m, meas)
        {
            if (out.measCount >= Profile::MAX_MEAS)
            {
                ESP_LOGW(TAG, "profile '%s': measurements[] truncated to %u (columns lost!)",
                         out.id, Profile::MAX_MEAS);
                break;
            }
            const cJSON *scpi = cJSON_GetObjectItemCaseSensitive(m, "scpi");
            const cJSON *label = cJSON_GetObjectItemCaseSensitive(m, "label");
            const cJSON *unit = cJSON_GetObjectItemCaseSensitive(m, "unit");
            const cJSON *col = cJSON_GetObjectItemCaseSensitive(m, "column");
            if (!cJSON_IsString(scpi))
                continue;
            setMeas(out.meas[out.measCount],
                    scpi->valuestring,
                    cJSON_IsString(label) ? label->valuestring : scpi->valuestring,
                    cJSON_IsString(unit) ? unit->valuestring : "",
                    cJSON_IsString(col) ? col->valuestring : "value");
            out.measCount++;
        }
    }

    cJSON_Delete(root);

    if (out.measCount == 0)
    {
        ESP_LOGW(TAG, "profile '%s' has no measurements", out.id);
        return false;
    }
    return true;
}

// --- SD scanning -----------------------------------------------------------

bool profileLoadFromSd(const char *dir, const char *idn, uint16_t vid, Profile &out)
{
    DIR *d = opendir(dir);
    if (!d)
    {
        // No /profiles directory is fine - we fall back to built-ins.
        return false;
    }

    bool found = false;
    struct dirent *ent = nullptr;
    char path[512]; // dir + '/' + up to a 255-char LFN name
    static char buf[3072]; // one JSON file at a time; static to spare the stack

    while (!found && (ent = readdir(d)) != nullptr)
    {
        const char *dot = std::strrchr(ent->d_name, '.');
        if (!dot || std::strcmp(dot, ".json") != 0)
        {
            continue;
        }

        std::snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        FILE *f = std::fopen(path, "r");
        if (!f)
        {
            continue;
        }
        size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        buf[n] = '\0';
        if (n == sizeof(buf) - 1)
        {
            // Bigger than the buffer - the parse below will most likely fail.
            ESP_LOGW(TAG, "%s exceeds %u bytes, truncated", ent->d_name,
                     static_cast<unsigned>(sizeof(buf)));
        }

        Profile tmp;
        if (profileParseJson(buf, tmp) && tmp.matches(idn, vid))
        {
            out = tmp;
            found = true;
            ESP_LOGI(TAG, "matched SD profile %s (%s)", ent->d_name, out.id);
        }
    }

    closedir(d);
    return found;
}
