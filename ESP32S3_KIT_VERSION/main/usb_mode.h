//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   usb_mode.h
/// @author Petr Vanek
///
/// Boot-time USB role of the kit. The ESP32-S3 has a single USB OTG
/// controller, so the two roles are exclusive and selected by rebooting:
///
///   Logger  - OTG in host role, instruments on the USB HOST port (default).
///   PC mode - OTG in device role, the SD card exposed to a PC as a USB
///             Mass Storage disk on the USB DEV port.
///
/// The choice lives in RTC noinit RAM: it survives esp_restart() (that is how
/// the mode switch works) but NOT a power cycle - after plugging the kit in it
/// always comes up as a logger, never "stuck" in PC mode. 

#pragma once

namespace usbmode
{
    /// True when this boot should run as a USB MSC disk (PC mode).
    bool isPcMode();

    /// Reboot into PC mode (USB disk). Caller must have ejected the SD first.
    [[noreturn]] void rebootToPcMode();

    /// Reboot back into the default logger mode.
    [[noreturn]] void rebootToLogger();
}
