//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   usbtmc.cpp
/// @author Petr Vanek

#include "usbtmc.h"
#include "usb/usb_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static const char *TAG = "USBTMC";

UsbtmcDevice::~UsbtmcDevice()
{
    close();
}

uint8_t UsbtmcDevice::nextTag()
{
    // bTag must be in 1..255 and different on each transaction.
    _tag = static_cast<uint8_t>(_tag + 1);
    if (_tag == 0)
    {
        _tag = 1;
    }
    return _tag;
}

bool UsbtmcDevice::findInterface(const usb_config_desc_t *cfg)
{
    const uint8_t *p = reinterpret_cast<const uint8_t *>(cfg);
    const uint8_t *end = p + cfg->wTotalLength;

    const uint8_t *q = p;
    while (q + 2 <= end)
    {
        uint8_t len = q[0];
        uint8_t type = q[1];
        if (len == 0)
        {
            break;
        }

        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE)
        {
            const usb_intf_desc_t *intf = reinterpret_cast<const usb_intf_desc_t *>(q);
            // Application-Specific class 0xFE, subclass 0x03 = USBTMC.
            // Protocol 0x00 (USBTMC) or 0x01 (USB488) both accepted.
            if (intf->bInterfaceClass == 0xFE && intf->bInterfaceSubClass == 0x03)
            {
                _intf = intf->bInterfaceNumber;
                _alt = intf->bAlternateSetting;
                _epOut = 0;
                _epIn = 0;

                // Collect this interface's bulk endpoints until the next
                // interface descriptor.
                const uint8_t *r = q + len;
                while (r + 2 <= end)
                {
                    uint8_t rl = r[0];
                    uint8_t rt = r[1];
                    if (rl == 0 || rt == USB_B_DESCRIPTOR_TYPE_INTERFACE)
                    {
                        break;
                    }
                    if (rt == USB_B_DESCRIPTOR_TYPE_ENDPOINT)
                    {
                        const usb_ep_desc_t *ep = reinterpret_cast<const usb_ep_desc_t *>(r);
                        bool isBulk = (ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) ==
                                      USB_BM_ATTRIBUTES_XFER_BULK;
                        if (isBulk)
                        {
                            if (ep->bEndpointAddress & 0x80)
                            {
                                _epIn = ep->bEndpointAddress;
                                _epInMps = ep->wMaxPacketSize;
                            }
                            else
                            {
                                _epOut = ep->bEndpointAddress;
                            }
                        }
                    }
                    r += rl;
                }

                ESP_LOGI(TAG, "USBTMC interface %u: bulk-OUT 0x%02X, bulk-IN 0x%02X (mps %u)",
                         _intf, _epOut, _epIn, _epInMps);
                return _epIn != 0 && _epOut != 0;
            }
        }
        q += len;
    }
    return false;
}

bool UsbtmcDevice::open(usb_host_client_handle_t client, usb_device_handle_t dev)
{
    if (!client || !dev)
    {
        return false;
    }
    _client = client;
    _dev = dev;

    const usb_config_desc_t *cfg = nullptr;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || !cfg)
    {
        ESP_LOGE(TAG, "get_active_config_descriptor failed");
        _dev = nullptr;
        return false;
    }

    if (!findInterface(cfg))
    {
        ESP_LOGW(TAG, "no USBTMC interface on this device");
        _dev = nullptr;
        return false;
    }

    esp_err_t err = usb_host_interface_claim(_client, _dev, _intf, _alt);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "interface_claim failed: %s", esp_err_to_name(err));
        _dev = nullptr;
        return false;
    }

    if (usb_host_transfer_alloc(OUT_BUF_SIZE, 0, &_outXfer) != ESP_OK ||
        usb_host_transfer_alloc(IN_BUF_SIZE, 0, &_inXfer) != ESP_OK ||
        usb_host_transfer_alloc(CTRL_BUF_SIZE, 0, &_ctrlXfer) != ESP_OK)
    {
        ESP_LOGE(TAG, "transfer_alloc failed");
        close();
        return false;
    }

    return true;
}

void UsbtmcDevice::close()
{
    if (_outXfer)
    {
        usb_host_transfer_free(_outXfer);
        _outXfer = nullptr;
    }
    if (_inXfer)
    {
        usb_host_transfer_free(_inXfer);
        _inXfer = nullptr;
    }
    if (_ctrlXfer)
    {
        usb_host_transfer_free(_ctrlXfer);
        _ctrlXfer = nullptr;
    }
    if (_dev)
    {
        usb_host_interface_release(_client, _dev, _intf);
    }
    _dev = nullptr;
    _client = nullptr;
    _epIn = _epOut = 0;
}

void UsbtmcDevice::transferCb(usb_transfer_t *xfer)
{
    auto *self = static_cast<UsbtmcDevice *>(xfer->context);
    self->_xferStatus = xfer->status;
    self->_xferActual = xfer->actual_num_bytes;
    self->_xferDone = true;
}

bool UsbtmcDevice::transferSync(usb_transfer_t *xfer, size_t numBytes,
                                uint8_t endpoint, uint32_t timeoutMs)
{
    _xferDone = false;
    _xferStatus = USB_TRANSFER_STATUS_ERROR;
    _xferActual = 0;

    xfer->device_handle = _dev;
    xfer->bEndpointAddress = endpoint;
    xfer->num_bytes = static_cast<int>(numBytes);
    xfer->callback = &UsbtmcDevice::transferCb;
    xfer->context = this;

    esp_err_t err = usb_host_transfer_submit(xfer);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "transfer_submit failed: %s", esp_err_to_name(err));
        return false;
    }

    // The completion callback is dispatched from usb_host_client_handle_events,
    // so we must keep pumping it while we wait.
    int64_t startUs = esp_timer_get_time();
    while (!_xferDone)
    {
        usb_host_client_handle_events(_client, pdMS_TO_TICKS(10));
        if ((esp_timer_get_time() - startUs) / 1000 > timeoutMs)
        {
            ESP_LOGW(TAG, "transfer timeout on ep 0x%02X", endpoint);
            return false;
        }
    }

    if (_xferStatus != USB_TRANSFER_STATUS_COMPLETED)
    {
        ESP_LOGW(TAG, "transfer ep 0x%02X status %d", endpoint, _xferStatus);
        // An errored/stalled bulk transfer leaves the pipe halted; the USB
        // stack rejects the next submit on it with ESP_ERR_INVALID_STATE.
        // Clear exactly this endpoint so a retry can go through.
        usb_host_endpoint_clear(_dev, endpoint);
        return false;
    }
    return true;
}

bool UsbtmcDevice::controlXfer(uint8_t bmReqType, uint8_t bRequest, uint16_t wValue,
                               uint16_t wIndex, uint8_t *data, uint16_t wLength,
                               uint32_t timeoutMs)
{
    if (sizeof(usb_setup_packet_t) + wLength > CTRL_BUF_SIZE)
    {
        return false;
    }

    auto *setup = reinterpret_cast<usb_setup_packet_t *>(_ctrlXfer->data_buffer);
    setup->bmRequestType = bmReqType;
    setup->bRequest = bRequest;
    setup->wValue = wValue;
    setup->wIndex = wIndex;
    setup->wLength = wLength;

    bool dirIn = (bmReqType & 0x80) != 0;
    if (!dirIn && wLength > 0 && data)
    {
        std::memcpy(_ctrlXfer->data_buffer + sizeof(usb_setup_packet_t), data, wLength);
    }

    _xferDone = false;
    _xferStatus = USB_TRANSFER_STATUS_ERROR;
    _xferActual = 0;
    _ctrlXfer->device_handle = _dev;
    _ctrlXfer->bEndpointAddress = 0;
    _ctrlXfer->num_bytes = static_cast<int>(sizeof(usb_setup_packet_t) + wLength);
    _ctrlXfer->callback = &UsbtmcDevice::transferCb;
    _ctrlXfer->context = this;

    esp_err_t err = usb_host_transfer_submit_control(_client, _ctrlXfer);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "control submit failed: %s", esp_err_to_name(err));
        return false;
    }

    int64_t startUs = esp_timer_get_time();
    while (!_xferDone)
    {
        usb_host_client_handle_events(_client, pdMS_TO_TICKS(10));
        if ((esp_timer_get_time() - startUs) / 1000 > timeoutMs)
        {
            ESP_LOGW(TAG, "control transfer timeout (req %u)", bRequest);
            return false;
        }
    }
    if (_xferStatus != USB_TRANSFER_STATUS_COMPLETED)
    {
        ESP_LOGW(TAG, "control transfer req %u status %d", bRequest, _xferStatus);
        return false;
    }

    if (dirIn && wLength > 0 && data)
    {
        int got = _xferActual - static_cast<int>(sizeof(usb_setup_packet_t));
        if (got < 0)
        {
            got = 0;
        }
        std::memcpy(data, _ctrlXfer->data_buffer + sizeof(usb_setup_packet_t),
                    got > wLength ? wLength : got);
    }
    return true;
}

bool UsbtmcDevice::deviceClear(uint32_t timeoutMs)
{
    if (!isOpen())
    {
        return false;
    }
    ESP_LOGI(TAG, "USBTMC device clear...");

    // INITIATE_CLEAR (class, interface, IN).
    uint8_t status = 0;
    if (!controlXfer(0xA1, REQ_INITIATE_CLEAR, 0, _intf, &status, 1, 1000))
    {
        return false;
    }
    if (status != TMC_STATUS_SUCCESS)
    {
        ESP_LOGW(TAG, "INITIATE_CLEAR status 0x%02X", status);
        return false;
    }

    // Poll CHECK_CLEAR_STATUS until the device finishes aborting.
    int64_t startUs = esp_timer_get_time();
    uint8_t chk[2] = {0, 0};
    do
    {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!controlXfer(0xA1, REQ_CHECK_CLEAR_STATUS, 0, _intf, chk, 2, 1000))
        {
            return false;
        }
        if ((esp_timer_get_time() - startUs) / 1000 > timeoutMs)
        {
            ESP_LOGW(TAG, "CHECK_CLEAR_STATUS still pending - giving up");
            return false;
        }
    } while (chk[0] == TMC_STATUS_PENDING);

    // Spec: after the clear the host must clear the bulk-OUT halt. Do the
    // device side (CLEAR_FEATURE ENDPOINT_HALT) and the host-side pipes.
    controlXfer(0x02, 0x01 /*CLEAR_FEATURE*/, 0 /*ENDPOINT_HALT*/, _epOut, nullptr, 0, 1000);
    usb_host_endpoint_clear(_dev, _epOut);
    usb_host_endpoint_clear(_dev, _epIn);
    _tag = 0; // start a fresh bTag sequence

    ESP_LOGI(TAG, "USBTMC device clear done");
    return true;
}

bool UsbtmcDevice::sendCommand(const char *cmd, uint32_t timeoutMs)
{
    size_t cmdLen = std::strlen(cmd);
    bool needsNl = (cmdLen == 0 || cmd[cmdLen - 1] != '\n');
    size_t payloadLen = cmdLen + (needsNl ? 1 : 0);

    // Header (12 B) + payload, whole message padded to a multiple of 4 B.
    size_t total = TMC_HEADER_LEN + payloadLen;
    size_t padded = (total + 3) & ~static_cast<size_t>(3);
    if (padded > OUT_BUF_SIZE)
    {
        ESP_LOGE(TAG, "command too long");
        return false;
    }

    uint8_t *b = _outXfer->data_buffer;
    std::memset(b, 0, padded);
    uint8_t tag = nextTag();
    b[0] = MSGID_DEV_DEP_MSG_OUT;
    b[1] = tag;
    b[2] = static_cast<uint8_t>(~tag);
    b[3] = 0;
    b[4] = static_cast<uint8_t>(payloadLen & 0xFF);
    b[5] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
    b[6] = static_cast<uint8_t>((payloadLen >> 16) & 0xFF);
    b[7] = static_cast<uint8_t>((payloadLen >> 24) & 0xFF);
    b[8] = 0x01; // bmTransferAttributes: EOM
    b[9] = 0;
    std::memcpy(b + TMC_HEADER_LEN, cmd, cmdLen);
    if (needsNl)
    {
        b[TMC_HEADER_LEN + cmdLen] = '\n';
    }

    return transferSync(_outXfer, padded, _epOut, timeoutMs);
}

bool UsbtmcDevice::requestResponse(size_t maxLen, uint32_t timeoutMs)
{
    uint8_t *b = _outXfer->data_buffer;
    std::memset(b, 0, TMC_HEADER_LEN);
    uint8_t tag = nextTag();
    b[0] = MSGID_REQUEST_DEV_DEP_MSG_IN;
    b[1] = tag;
    b[2] = static_cast<uint8_t>(~tag);
    b[3] = 0;
    b[4] = static_cast<uint8_t>(maxLen & 0xFF);
    b[5] = static_cast<uint8_t>((maxLen >> 8) & 0xFF);
    b[6] = static_cast<uint8_t>((maxLen >> 16) & 0xFF);
    b[7] = static_cast<uint8_t>((maxLen >> 24) & 0xFF);
    b[8] = 0x00; // bmTransferAttributes: no TermChar
    b[9] = 0x00; // TermChar
    return transferSync(_outXfer, TMC_HEADER_LEN, _epOut, timeoutMs);
}

bool UsbtmcDevice::readResponse(uint32_t timeoutMs)
{
    // Bulk-IN transfers must be a multiple of the endpoint max packet size.
    size_t want = IN_BUF_SIZE - (IN_BUF_SIZE % _epInMps);
    if (want == 0)
    {
        want = _epInMps;
    }
    return transferSync(_inXfer, want, _epIn, timeoutMs);
}

bool UsbtmcDevice::query(const char *cmd, char *out, size_t outCap,
                         size_t *outLen, uint32_t timeoutMs)
{
    if (!isOpen() || !out || outCap == 0)
    {
        return false;
    }

    if (!sendCommand(cmd, timeoutMs))
    {
        return false;
    }
    if (!requestResponse(IN_BUF_SIZE - TMC_HEADER_LEN, timeoutMs))
    {
        return false;
    }
    if (!readResponse(timeoutMs))
    {
        return false;
    }

    if (_xferActual < static_cast<int>(TMC_HEADER_LEN))
    {
        ESP_LOGW(TAG, "short response (%d bytes)", _xferActual);
        return false;
    }

    const uint8_t *b = _inXfer->data_buffer;
    // Response header: bMsgID should echo DEV_DEP_MSG_IN (2).
    uint32_t dataLen = static_cast<uint32_t>(b[4]) |
                       (static_cast<uint32_t>(b[5]) << 8) |
                       (static_cast<uint32_t>(b[6]) << 16) |
                       (static_cast<uint32_t>(b[7]) << 24);

    size_t avail = static_cast<size_t>(_xferActual) - TMC_HEADER_LEN;
    if (dataLen > avail)
    {
        dataLen = avail; // never read past what actually arrived
    }
    if (dataLen > outCap - 1)
    {
        dataLen = outCap - 1;
    }

    std::memcpy(out, b + TMC_HEADER_LEN, dataLen);
    // Trim a trailing CR/LF so the UI gets a clean string.
    while (dataLen > 0 && (out[dataLen - 1] == '\n' || out[dataLen - 1] == '\r'))
    {
        dataLen--;
    }
    out[dataLen] = '\0';
    if (outLen)
    {
        *outLen = dataLen;
    }
    return true;
}

bool UsbtmcDevice::writeCommand(const char *cmd, uint32_t timeoutMs)
{
    if (!isOpen())
    {
        return false;
    }
    return sendCommand(cmd, timeoutMs);
}
