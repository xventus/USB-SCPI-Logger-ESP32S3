//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   web_server.h
/// @author Petr Vanek
///
/// HTTP server for the Soft AP (started/stopped by the WifiTask). Serves the
/// static page from the LittleFS "web" partition and the REST API the page
/// expects (contract in PROGRESS.md):
///
///   GET  /api/status            state + latest readings (JSON)
///   GET  /api/files             list of /sdcard/logs CSVs (JSON)
///   GET  /api/files/<name>      CSV download
///   GET  /api/profiles          list of /sdcard/profiles JSONs
///   GET  /api/profiles/<name>   profile download
///   PUT  /api/profiles/<name>   profile upload (body = file content)
///   POST /api/control           {"action":"start"|"stop"}
///
/// SD reads rely on the FATFS lock; the CSV being written is at most one
/// fsync interval (5 rows / 3 s) behind.

#pragma once

/// Start the HTTP server (call with the AP up). False on failure.
bool webServerStart();

/// Stop the HTTP server (frees its memory).
void webServerStop();
