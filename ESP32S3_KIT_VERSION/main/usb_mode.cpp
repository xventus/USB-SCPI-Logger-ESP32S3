//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   usb_mode.cpp
/// @author Petr Vanek

#include "usb_mode.h"
#include "esp_attr.h"
#include "esp_system.h"
#include <cstdint>

// RTC noinit: kept across esp_restart(), garbage after power-on - exactly the
// lifetime we want (see usb_mode.h). The magic makes garbage read as Logger.
static RTC_NOINIT_ATTR uint32_t s_bootMode;

static constexpr uint32_t MODE_PC_MAGIC = 0x50434D44; // "PCMD"

namespace usbmode
{
    bool isPcMode()
    {
        return s_bootMode == MODE_PC_MAGIC;
    }

    void rebootToPcMode()
    {
        s_bootMode = MODE_PC_MAGIC;
        esp_restart();
    }

    void rebootToLogger()
    {
        s_bootMode = 0;
        esp_restart();
    }
}
