//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   hardware.h
/// @author Petr Vanek
///
/// Hardware notes for the Espressif ESP32-S3-USB-OTG dev kit - TODO: own board
///
/// The concrete pin map (ST7789 LCD on SPI + backlight GPIO9, the four
/// buttons, the SD slot and the USB host VBUS control GPIOs) lives inside the
/// board support package `espressif/esp32_s3_usb_otg`. The BSP owns them and exposes typed handles and
/// helper functions instead. This header only collects the board-level facts
/// that our application code needs to reason about.

#pragma once

#include "bsp/esp32_s3_usb_otg.h"

// ---------------------------------------------------------------------------
// USB host power
// ---------------------------------------------------------------------------
// The Type-A host connector (where the instrument is plugged in) CANNOT be
// powered from the debug micro-USB port - the board must be powered
// separately. VBUS for the host is enabled through the BSP:
//
//   bsp_usb_host_start(HW_USB_HOST_POWER_MODE, HW_USB_HOST_LIMIT_500MA);
//
// USB_DEV mode routes VBUS from the device-side USB input; use BATTERY mode
// when running from the on-board Li-ion cell. TODO: Battery will be removed in production 
#define HW_USB_HOST_POWER_MODE   BSP_USB_HOST_POWER_MODE_USB_DEV
#define HW_USB_HOST_LIMIT_500MA  true

// ---------------------------------------------------------------------------
// USBTMC device class 
// ---------------------------------------------------------------------------
// A USBTMC instrument advertises itself as an Application-Specific interface.
// The exact endpoint addresses are read from the live enumeration, never
// hard-coded.
#define HW_USBTMC_CLASS      0xFE  ///< bInterfaceClass  (Application Specific)
#define HW_USBTMC_SUBCLASS   0x03  ///< bInterfaceSubClass (USBTMC)
// bInterfaceProtocol: 0x00 = USBTMC, 0x01 = USB488 - both accepted.

// ---------------------------------------------------------------------------
// Board buttons
// ---------------------------------------------------------------------------
// Index of each button in the array filled by bsp_iot_button_create() - the
// order follows the BSP's bsp_button_t enum. The GPIO numbers themselves live
// in the BSP.
#define HW_BTN_IDX_OK    0
#define HW_BTN_IDX_DOWN  1
#define HW_BTN_IDX_UP    2
#define HW_BTN_IDX_MENU  3
