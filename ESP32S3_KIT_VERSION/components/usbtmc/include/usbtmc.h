//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   usbtmc.h
/// @author Petr Vanek
///
/// USBTMC bulk transport over the generic usb_host client API.
///
/// Usage (single-threaded, from the task that pumps
/// usb_host_client_handle_events):
///
///   UsbtmcDevice tmc;
///   if (tmc.open(client, dev)) {
///       char idn[128]; size_t n = 0;
///       if (tmc.query("*IDN?", idn, sizeof(idn), &n)) { ... }
///   }
///   tmc.close();
///
/// IMPORTANT: query()/writeCommand() must NOT be called from inside a
/// usb_host client event callback - they pump usb_host_client_handle_events()
/// themselves while waiting for the bulk transfers to complete, which would
/// re-enter the event handler. Call them from the main task loop instead.

#pragma once

#include "usb/usb_host.h"
#include <cstddef>
#include <cstdint>

class UsbtmcDevice
{
public:
    UsbtmcDevice() = default;
    ~UsbtmcDevice();

    UsbtmcDevice(const UsbtmcDevice &) = delete;
    UsbtmcDevice &operator=(const UsbtmcDevice &) = delete;

    /// Claim the USBTMC interface (bInterfaceClass 0xFE / subclass 0x03) of an
    /// already-opened device and locate its bulk-IN/OUT endpoints from the
    /// live configuration descriptor. Returns false if the device exposes no
    /// USBTMC interface (e.g. a keyboard) - not an error, just "not TMC".
    bool open(usb_host_client_handle_t client, usb_device_handle_t dev);

    /// Release the interface and free the transfer buffers.
    void close();

    bool isOpen() const { return _dev != nullptr; }

    /// Send a SCPI command and read the response text. A trailing newline is
    /// appended to @p cmd automatically. @p out is always null-terminated on
    /// success; @p outLen (optional) receives the number of payload bytes.
    bool query(const char *cmd, char *out, size_t outCap, size_t *outLen,
               uint32_t timeoutMs = 1000);

    /// Send a SCPI command without reading a response (e.g. init commands).
    bool writeCommand(const char *cmd, uint32_t timeoutMs = 1000);

    /// USBTMC INITIATE_CLEAR + CHECK_CLEAR_STATUS: abort whatever bulk
    /// transfer the instrument is stuck in and clear both pipes. This is THE
    /// recovery for self-powered instruments (DP832A) where a VBUS power
    /// cycle does nothing - their USBTMC state machine survives it wedged.
    bool deviceClear(uint32_t timeoutMs = 2000);

private:
    // USBTMC bulk header fields 
    static constexpr uint8_t MSGID_DEV_DEP_MSG_OUT = 1;
    static constexpr uint8_t MSGID_REQUEST_DEV_DEP_MSG_IN = 2;
    static constexpr size_t TMC_HEADER_LEN = 12;
    static constexpr size_t OUT_BUF_SIZE = 128;  ///< header + short SCPI cmd
    static constexpr size_t IN_BUF_SIZE = 512;   ///< header + response text
    static constexpr size_t CTRL_BUF_SIZE = 16;  ///< setup packet + status bytes

    // USBTMC class control requests (spec rev 1.0, table 15).
    static constexpr uint8_t REQ_INITIATE_CLEAR = 5;
    static constexpr uint8_t REQ_CHECK_CLEAR_STATUS = 6;
    static constexpr uint8_t TMC_STATUS_SUCCESS = 0x01;
    static constexpr uint8_t TMC_STATUS_PENDING = 0x02;

    bool findInterface(const usb_config_desc_t *cfg);
    uint8_t nextTag();

    bool sendCommand(const char *cmd, uint32_t timeoutMs);
    bool requestResponse(size_t maxLen, uint32_t timeoutMs);
    bool readResponse(uint32_t timeoutMs);
    bool transferSync(usb_transfer_t *xfer, size_t numBytes, uint8_t endpoint,
                      uint32_t timeoutMs);

    /// Synchronous control transfer. IN requests (bit 7 of @p bmReqType set)
    /// copy up to @p wLength bytes into @p data.
    bool controlXfer(uint8_t bmReqType, uint8_t bRequest, uint16_t wValue,
                     uint16_t wIndex, uint8_t *data, uint16_t wLength,
                     uint32_t timeoutMs);

    static void transferCb(usb_transfer_t *xfer);

    usb_host_client_handle_t _client{nullptr};
    usb_device_handle_t _dev{nullptr};

    uint8_t _intf{0};
    uint8_t _alt{0};
    uint8_t _epOut{0};
    uint8_t _epIn{0};
    uint16_t _epInMps{64};
    uint8_t _tag{0};

    usb_transfer_t *_outXfer{nullptr};
    usb_transfer_t *_inXfer{nullptr};
    usb_transfer_t *_ctrlXfer{nullptr};

    // Set by transferCb (same task context - no locking needed).
    volatile bool _xferDone{false};
    usb_transfer_status_t _xferStatus{USB_TRANSFER_STATUS_ERROR};
    int _xferActual{0};
};
