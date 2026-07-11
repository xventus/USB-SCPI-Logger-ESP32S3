//
// vim: ts=4 et
// Copyright (c) 2026Petr Vanek, petr@fotoventus.cz
//
/// @file   msc_device.h
/// @author Petr Vanek
///
/// PC mode (USB dongle): switches the board's USB mux to the DEV connector,
/// initialises the SD card WITHOUT mounting its filesystem locally and exposes
/// it to the PC as a USB Mass Storage disk via TinyUSB.
///
/// The firmware must never have the FAT mounted while the PC owns the raw
/// blocks - two FAT drivers over one card corrupt it. That is why this runs
/// only in the dedicated PC boot mode (usb_mode.h) where the StorageTask (and
/// the USB host stack) are never started.
/// TODO: REWORK

#pragma once

/// Bring up the USB MSC disk. Returns false when the SD card is missing or
/// the TinyUSB stack failed to start.
bool mscDeviceStart();

/// True when the SD card initialised and is exposed to the PC (icon bar).
bool mscDeviceCardReady();
