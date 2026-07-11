//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   profile.h
/// @author Petr Vanek
///
/// Data-driven instrument description. One firmware, many
/// instruments: each instrument is a Profile (SCPI queries, column names,
/// interval) selected automatically after *IDN? by its `match` rule.

#pragma once

#include <cstdint>
#include <cstddef>

struct Measurement
{
    char scpi[48];   ///< SCPI query, e.g. ":MEASure:VOLTage:DC?"
    char label[24];  ///< UI label, e.g. "DC Voltage"
    char unit[12];   ///< unit, e.g. "V"
    char column[24]; ///< CSV column name, e.g. "voltage_dc"
};

struct Profile
{
    static constexpr uint8_t MAX_MEAS = 12; ///< column cap (covers DP932A U/I/P x 3ch;)
    static constexpr uint8_t MAX_INIT = 8;

    char id[32];
    char name[48];

    // match
    char idnContains[32]; ///< substring required in *IDN? ("" = any)
    uint16_t vid;         ///< required USB VID (0 = any)

    Measurement meas[MAX_MEAS];
    uint8_t measCount;

    char init[MAX_INIT][48]; ///< SCPI commands sent once on connect
    uint8_t initCount;

    uint32_t defaultIntervalMs;
    bool selectSingle; ///< true: log one measurement at a time (DMM function)
    bool autostart;    ///< start logging right on connect (false: OK / web)

    /// True if this profile matches the given identity.
    bool matches(const char *idn, uint16_t theVid) const;
};

/// Pick a built-in profile for the given identity, or nullptr if none match.
const Profile *profileMatchBuiltin(const char *idn, uint16_t vid);

/// Scan /sdcard/profiles/*.json for a matching profile. On success fills
/// @p out and returns true. SD profiles take precedence over built-ins.
bool profileLoadFromSd(const char *dir, const char *idn, uint16_t vid, Profile &out);

/// Parse a single JSON profile text into @p out. Returns true on success.
bool profileParseJson(const char *json, Profile &out);
