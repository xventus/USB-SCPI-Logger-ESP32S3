//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   display_task.cpp
/// @author Petr Vanek

#include "display_task.h"
#include "application.h"
#include "usb_mode.h"
#include "msc_device.h"
#include "hardware.h"
#include "bsp/esp32_s3_usb_otg.h"
#include "iot_button.h"
#include "esp_log.h"
#include "lvgl.h"
#include <cstring>
#include <cstdio>

DisplayTask::DisplayTask()
{
    _queue = xQueueCreate(8, sizeof(Msg));
}

DisplayTask::~DisplayTask()
{
    done();
    if (_queue)
    {
        vQueueDelete(_queue);
    }
}

void DisplayTask::postText(Msg::Kind kind, const char *text, bool highlight)
{
    if (!_queue)
    {
        return;
    }
    Msg msg{};
    msg.kind = kind;
    msg.highlight = highlight;
    if (text)
    {
        std::strncpy(msg.text, text, TEXT_MAX - 1);
    }
    xQueueSend(_queue, &msg, 0);
}

void DisplayTask::setStatus(const char *text, bool highlight)
{
    postText(Msg::Kind::Status, text, highlight);
}

void DisplayTask::setValue(const char *text)
{
    postText(Msg::Kind::Value, text);
}

void DisplayTask::setFile(const char *text)
{
    postText(Msg::Kind::File, text);
}

void DisplayTask::refreshMenu()
{
    if (!_queue)
    {
        return;
    }
    Msg msg{};
    msg.kind = Msg::Kind::Refresh;
    xQueueSend(_queue, &msg, 0);
}

// --- Button callbacks (iot_button task context: only queue an event) -------

void DisplayTask::cbMenu(void *, void *arg)
{
    auto *self = static_cast<DisplayTask *>(arg);
    Msg m{};
    m.kind = Msg::Kind::Button;
    m.button = Button::Menu;
    if (self->_queue)
        xQueueSend(self->_queue, &m, 0);
}

void DisplayTask::cbUp(void *, void *arg)
{
    auto *self = static_cast<DisplayTask *>(arg);
    Msg m{};
    m.kind = Msg::Kind::Button;
    m.button = Button::Up;
    if (self->_queue)
        xQueueSend(self->_queue, &m, 0);
}

void DisplayTask::cbDown(void *, void *arg)
{
    auto *self = static_cast<DisplayTask *>(arg);
    Msg m{};
    m.kind = Msg::Kind::Button;
    m.button = Button::Down;
    if (self->_queue)
        xQueueSend(self->_queue, &m, 0);
}

void DisplayTask::cbOk(void *, void *arg)
{
    auto *self = static_cast<DisplayTask *>(arg);
    Msg m{};
    m.kind = Msg::Kind::Button;
    m.button = Button::Ok;
    if (self->_queue)
        xQueueSend(self->_queue, &m, 0);
}

bool DisplayTask::initButtons()
{
    button_handle_t btns[BSP_BUTTON_NUM] = {};
    int cnt = 0;
    esp_err_t err = bsp_iot_button_create(btns, &cnt, BSP_BUTTON_NUM);
    if (err != ESP_OK)
    {
        ESP_LOGW(LOG_TAG, "button create failed: %s", esp_err_to_name(err));
        return false;
    }

    iot_button_register_cb(btns[HW_BTN_IDX_OK], BUTTON_SINGLE_CLICK, nullptr, &DisplayTask::cbOk, this);
    iot_button_register_cb(btns[HW_BTN_IDX_DOWN], BUTTON_SINGLE_CLICK, nullptr, &DisplayTask::cbDown, this);
    iot_button_register_cb(btns[HW_BTN_IDX_UP], BUTTON_SINGLE_CLICK, nullptr, &DisplayTask::cbUp, this);
    iot_button_register_cb(btns[HW_BTN_IDX_MENU], BUTTON_SINGLE_CLICK, nullptr, &DisplayTask::cbMenu, this);
    ESP_LOGI(LOG_TAG, "buttons ready (MENU/UP/DOWN/OK)");
    return true;
}

bool DisplayTask::buildUi()
{
    lv_display_t *disp = bsp_display_start();
    if (!disp)
    {
        ESP_LOGE(LOG_TAG, "bsp_display_start() failed");
        return false;
    }

    if (!bsp_display_lock(0))
    {
        ESP_LOGE(LOG_TAG, "bsp_display_lock() failed");
        return false;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, literals::ui_title);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00AAFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 8);

    // Status icon bar, right-aligned; stays visible on both screens. Order
    // from the right edge: run/pause, SD card, USB.
    auto makeIcon = [scr](int xOfs) {
        lv_obj_t *ico = lv_label_create(scr);
        lv_obj_set_style_text_font(ico, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(ico, lv_color_hex(0x555555), 0);
        lv_label_set_text(ico, "");
        lv_obj_align(ico, LV_ALIGN_TOP_RIGHT, xOfs, 6);
        return ico;
    };
    _icoRun = makeIcon(-8);
    _icoSd = makeIcon(-34);
    _icoUsb = makeIcon(-60);
    _icoWifi = makeIcon(-86);

    lv_obj_t *status = lv_label_create(scr);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status, lv_pct(95));
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status, lv_color_white(), 0);
    lv_label_set_text(status, literals::ui_starting);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 38);
    _lblStatus = status;

    // The live reading - the main thing on the screen, so the big font.
    lv_obj_t *value = lv_label_create(scr);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(value, lv_pct(95));
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(value, lv_color_hex(0x33FF66), 0);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_24, 0);
    lv_label_set_text(value, "");
    lv_obj_align(value, LV_ALIGN_CENTER, 0, 16);
    _lblValue = value;

    // Current CSV file - takes the hint's spot at the bottom while logging.
    lv_obj_t *file = lv_label_create(scr);
    lv_obj_set_width(file, lv_pct(95));
    lv_obj_set_style_text_align(file, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(file, lv_color_hex(0xAAAA66), 0);
    lv_label_set_text(file, "");
    lv_obj_align(file, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_flag(file, LV_OBJ_FLAG_HIDDEN);
    _lblFile = file;

    // Button hint for the Live screen's direct actions.
    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_width(hint, lv_pct(95));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(hint, literals::ui_hint_live);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    _lblHint = hint;

    // Menu block - a single multi-line label, hidden until MENU is pressed.
    // Recolor: the selected line renders green, the footer blue.
    lv_obj_t *menu = lv_label_create(scr);
    lv_label_set_long_mode(menu, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(menu, true);
    lv_obj_set_width(menu, lv_pct(95));
    lv_obj_set_style_text_color(menu, lv_color_white(), 0);
    lv_obj_set_style_text_font(menu, &lv_font_montserrat_16, 0);
    lv_label_set_text(menu, "");
    lv_obj_align(menu, LV_ALIGN_TOP_LEFT, 8, 34);
    lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN);
    _lblMenu = menu;

    bsp_display_unlock();

    // bsp_display_start() only initialises the backlight PWM (duty = 0).
    bsp_display_backlight_on();
    return true;
}

void DisplayTask::updateIcons()
{
    // Colors: green = active/OK, orange = paused, red = missing, blue = PC
    // link, dark grey = inactive.
    lv_color_t colOff = lv_color_hex(0x555555);

    const char *runSym = LV_SYMBOL_PAUSE;
    lv_color_t runCol = colOff;
    const char *sdSym = LV_SYMBOL_SD_CARD;
    lv_color_t sdCol;
    const char *usbSym = LV_SYMBOL_USB;
    lv_color_t usbCol;
    const char *wifiSym = LV_SYMBOL_WIFI;
    lv_color_t wifiCol = colOff;

    if (usbmode::isPcMode())
    {
        // USB disk for the PC: no measurement, no WiFi; SD belongs to the PC.
        runSym = "";
        wifiSym = "";
        usbCol = lv_color_hex(0x00AAFF);
        sdCol = mscDeviceCardReady() ? lv_color_hex(0x33FF66) : lv_color_hex(0xFF4444);
    }
    else
    {
        InstrumentTask::UiState ui{};
        Application::getInstance()->getInstrumentTask()->getUiState(ui);
        bool sdReady = Application::getInstance()->getStorageTask()->isStorageReady();

        if (ui.deviceReady && ui.running)
        {
            runSym = LV_SYMBOL_PLAY;
            runCol = lv_color_hex(0x33FF66);
        }
        else if (ui.deviceReady)
        {
            runCol = lv_color_hex(0xFFAA00); // instrument here, logging paused
        }
        usbCol = ui.deviceReady ? lv_color_hex(0x33FF66) : colOff;
        sdCol = sdReady ? lv_color_hex(0xCCCCCC) : lv_color_hex(0xFF4444);
        if (Application::getInstance()->getWifiTask()->apActive())
            wifiCol = lv_color_hex(0x33FF66);
    }

    if (!bsp_display_lock(100))
    {
        return;
    }
    lv_label_set_text(static_cast<lv_obj_t *>(_icoRun), runSym);
    lv_obj_set_style_text_color(static_cast<lv_obj_t *>(_icoRun), runCol, 0);
    lv_label_set_text(static_cast<lv_obj_t *>(_icoSd), sdSym);
    lv_obj_set_style_text_color(static_cast<lv_obj_t *>(_icoSd), sdCol, 0);
    lv_label_set_text(static_cast<lv_obj_t *>(_icoUsb), usbSym);
    lv_obj_set_style_text_color(static_cast<lv_obj_t *>(_icoUsb), usbCol, 0);
    lv_label_set_text(static_cast<lv_obj_t *>(_icoWifi), wifiSym);
    lv_obj_set_style_text_color(static_cast<lv_obj_t *>(_icoWifi), wifiCol, 0);
    bsp_display_unlock();
}

void DisplayTask::updateFileLine()
{
    if (!_fileVisible)
        return;

    // "file.csv  1:23:45" - elapsed time of the current logging session
    // (matches the CSV relative timestamps).
    uint32_t s = Application::getInstance()->getInstrumentTask()->sessionSeconds();
    char line[96];
    if (s >= 3600)
        std::snprintf(line, sizeof(line), literals::fmt_file_time_h, _fileName,
                      (unsigned)(s / 3600), (unsigned)(s % 3600 / 60), (unsigned)(s % 60));
    else
        std::snprintf(line, sizeof(line), literals::fmt_file_time_m, _fileName,
                      (unsigned)(s / 60), (unsigned)(s % 60));

    if (!bsp_display_lock(100))
        return;
    lv_label_set_text(static_cast<lv_obj_t *>(_lblFile), line);
    bsp_display_unlock();
}

void DisplayTask::showLive()
{
    if (!bsp_display_lock(100))
    {
        return;
    }
    lv_obj_add_flag(static_cast<lv_obj_t *>(_lblMenu), LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(static_cast<lv_obj_t *>(_lblStatus), LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(static_cast<lv_obj_t *>(_lblValue), LV_OBJ_FLAG_HIDDEN);
    // The bottom line is either the logged-file name or the button hint -
    // they share the spot (MENU being the menu button needs no reminder).
    if (_fileVisible)
    {
        lv_obj_remove_flag(static_cast<lv_obj_t *>(_lblFile), LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(static_cast<lv_obj_t *>(_lblHint), LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(static_cast<lv_obj_t *>(_lblFile), LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(static_cast<lv_obj_t *>(_lblHint), LV_OBJ_FLAG_HIDDEN);
    }
    bsp_display_unlock();
}

void DisplayTask::renderMenu()
{
    //TODO: rework !!!
    InstrumentTask::UiState ui{};
    Application::getInstance()->getInstrumentTask()->getUiState(ui);

    // Interval as a readable label.
    char interval[16];
    if (ui.intervalMs >= 1000 && (ui.intervalMs % 1000) == 0)
        std::snprintf(interval, sizeof(interval), literals::fmt_interval_s,
                      (unsigned long)(ui.intervalMs / 1000));
    else
        std::snprintf(interval, sizeof(interval), literals::fmt_interval_frac,
                      ui.intervalMs / 1000.0);

    const char *quantity = ui.deviceReady ? ui.activeLabel : literals::ui_none;

    // SD status snapshot for the menu line. OK on the item toggles eject
    // (safe remove) / mount. Free space only, in GB 
    char sd[48];
    auto info = Application::getInstance()->getStorageTask()->sdInfo();
    if (info.mounted)
    {
        double freeGb = static_cast<double>(info.freeBytes) / (1024.0 * 1024.0 * 1024.0);
        std::snprintf(sd, sizeof(sd), literals::fmt_sd_info, freeGb);
    }
    else
    {
        std::strncpy(sd, literals::ui_sd_ejected, sizeof(sd) - 1);
        sd[sizeof(sd) - 1] = '\0';
    }

    bool apOn = Application::getInstance()->getWifiTask()->apActive();

    // One text per item, then the selected one is wrapped in green recolor
    char items[static_cast<size_t>(MenuItem::COUNT)][56];
    std::snprintf(items[0], sizeof(items[0]), literals::fmt_menu_interval, interval);
    std::snprintf(items[1], sizeof(items[1]), literals::fmt_menu_quantity, quantity);
    std::snprintf(items[2], sizeof(items[2]), literals::fmt_menu_logging,
                  ui.running ? literals::ui_on : literals::ui_off);
    std::snprintf(items[3], sizeof(items[3]), literals::fmt_menu_sd, sd);
    std::snprintf(items[4], sizeof(items[4]), literals::fmt_menu_wifi,
                  apOn ? literals::ui_on : literals::ui_off);
    std::snprintf(items[5], sizeof(items[5]), "%s", literals::ui_menu_pcmode);

    char buf[512];
    int pos = std::snprintf(buf, sizeof(buf), "%s\n\n", literals::ui_menu_title);
    for (uint8_t i = 0; i < static_cast<uint8_t>(MenuItem::COUNT); ++i)
    {
        pos += std::snprintf(buf + pos, sizeof(buf) - pos,
                             i == _menuSel ? literals::fmt_menu_item_sel
                                           : literals::fmt_menu_item,
                             items[i]);
        if (pos >= static_cast<int>(sizeof(buf)))
            break;
    }
    if (pos < static_cast<int>(sizeof(buf)))
        std::snprintf(buf + pos, sizeof(buf) - pos, "%s", literals::ui_menu_footer);

    if (!bsp_display_lock(100))
    {
        return;
    }
    lv_label_set_text(static_cast<lv_obj_t *>(_lblMenu), buf);
    lv_obj_remove_flag(static_cast<lv_obj_t *>(_lblMenu), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(static_cast<lv_obj_t *>(_lblStatus), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(static_cast<lv_obj_t *>(_lblValue), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(static_cast<lv_obj_t *>(_lblFile), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(static_cast<lv_obj_t *>(_lblHint), LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

void DisplayTask::activateItem()
{
    auto *inst = Application::getInstance()->getInstrumentTask();
    if (!inst)
        return;
    switch (static_cast<MenuItem>(_menuSel))
    {
    case MenuItem::Interval:
        inst->cycleInterval();
        break;
    case MenuItem::Quantity:
        inst->cycleQuantity();
        break;
    case MenuItem::Logging:
        inst->toggleRunning();
        break;
    case MenuItem::SdInfo:
    {
        // Toggle safe-remove: eject closes the CSV + unmounts; mount re-mounts
        // and restarts the logging session so rows land in a fresh file.
        auto *stor = Application::getInstance()->getStorageTask();
        if (stor->sdInfo().mounted)
        {
            stor->requestEject();
        }
        else
        {
            stor->requestMount();       // queued first...
            inst->restartLogging();     // ...so the new session finds the card
        }
        break;
    }
    case MenuItem::Wifi:
        Application::getInstance()->getWifiTask()->toggle();
        break;
    case MenuItem::PcMode:
        enterPcMode();
        break;
    default:
        break;
    }
}

void DisplayTask::enterPcMode()
{
    // Safe hand-over of the card to the PC: close the CSV + unmount first,
    // then reboot into the MSC boot mode. Blocking the UI here is fine - we
    // are about to restart anyway.
    // Poll the cheap mounted flag, NOT sdInfo(): esp_vfs_fat_info() runs a
    // full FAT free-cluster scan on a freshly mounted card (seconds!) and
    // fights the unmount for the FATFS lock.
    auto *stor = Application::getInstance()->getStorageTask();
    stor->requestEject();

    for (int i = 0; i < 30 && stor->isStorageReady(); ++i)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (stor->isStorageReady())
    {
        // Eject did not finish (SD stuck mid-write?) - do NOT reboot over an
        // open file; let the user retry.
        setStatus(literals::ui_sd_busy);
        return;
    }
    usbmode::rebootToPcMode();
}

void DisplayTask::onButton(Button b)
{
    // PC boot mode: the kit is a USB disk, the only action is going back.
    if (usbmode::isPcMode())
    {
        if (b == Button::Up)
            usbmode::rebootToLogger();
        return;
    }

    const uint8_t count = static_cast<uint8_t>(MenuItem::COUNT);
    if (_screen == Screen::Live)
    {
        auto *inst = Application::getInstance()->getInstrumentTask();
        switch (b)
        {
        case Button::Menu:
            _screen = Screen::Menu;
            _menuSel = 0;
            renderMenu();
            break;
        case Button::Ok: // direct start/stop of the measurement
            if (inst)
                inst->toggleRunning();
            break;
        case Button::Up: // become a USB disk for the PC
            enterPcMode();
            break;
        case Button::Down: // WiFi Soft AP on/off
            Application::getInstance()->getWifiTask()->toggle();
            break;
        }
        return;
    }

    // Menu screen.
    switch (b)
    {
    case Button::Menu:
        _screen = Screen::Live;
        showLive();
        break;
    case Button::Up:
        _menuSel = (_menuSel + count - 1) % count;
        renderMenu();
        break;
    case Button::Down:
        _menuSel = (_menuSel + 1) % count;
        renderMenu();
        break;
    case Button::Ok:
        activateItem();
        renderMenu();
        break;
    }
}

void DisplayTask::loop()
{
    bool ok = buildUi();
    initButtons();
    Application::getInstance()->signalTaskStart(Application::TaskBit::Display);

    if (!ok)
    {
        ESP_LOGE(LOG_TAG, "Display init failed, task idling.");
        while (true)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    updateIcons();

    Msg msg{};
    while (true)
    {
        // Timeout doubles as the refresh tick for the icon bar and the
        // elapsed-time line (cheap reads only).
        if (xQueueReceive(_queue, &msg, pdMS_TO_TICKS(500)) != pdTRUE)
        {
            updateIcons();
            if (_screen == Screen::Live)
                updateFileLine();
            continue;
        }

        switch (msg.kind)
        {
        case Msg::Kind::Status:
        case Msg::Kind::Value:
        {
            lv_obj_t *target = static_cast<lv_obj_t *>(
                msg.kind == Msg::Kind::Status ? _lblStatus : _lblValue);
            if (target && bsp_display_lock(100))
            {
                if (msg.kind == Msg::Kind::Value)
                {
                    // One reading = big font; a multi-line block (DP832A
                    // channels under each other) = smaller font to fit.
                    lv_obj_set_style_text_font(target,
                                               std::strchr(msg.text, '\n')
                                                   ? &lv_font_montserrat_16
                                                   : &lv_font_montserrat_24,
                                               0);
                }
                else
                {
                    // Status: yellow marks a profile loaded from the SD card.
                    lv_obj_set_style_text_color(target,
                                                msg.highlight ? lv_color_hex(0xFFD400)
                                                              : lv_color_white(),
                                                0);
                }
                lv_label_set_text(target, msg.text);
                bsp_display_unlock();
            }
            break;
        }

        case Msg::Kind::File:
            // File name and button hint share the bottom line; the name is
            // kept so the periodic tick can append the elapsed time.
            std::strncpy(_fileName, msg.text, sizeof(_fileName) - 1);
            _fileName[sizeof(_fileName) - 1] = '\0';
            _fileVisible = (_fileName[0] != '\0');
            updateFileLine();
            if (_screen == Screen::Live)
                showLive();
            break;
        case Msg::Kind::Button:
            onButton(msg.button);
            break;

        case Msg::Kind::Refresh:
            // Another task finished a menu action - repaint with fresh state.
            updateIcons();
            if (_screen == Screen::Menu)
                renderMenu();
            break;
        }
    }
}
