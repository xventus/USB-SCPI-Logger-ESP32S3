//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   display_task.h
/// @author Petr Vanek
///
/// LVGL UI task for the ST7789 LCD on the ESP32-S3-USB-OTG board.

#pragma once

#include "rptask.h"

class DisplayTask : public RPTask
{
public:
    DisplayTask();
    virtual ~DisplayTask();

    /// Set the status line (top). Thread-safe, non-blocking.
    /// @p highlight renders it yellow - used for a profile loaded from the
    /// SD card (as opposed to a firmware built-in).
    void setStatus(const char *text, bool highlight = false);

    /// Set the large value line (center) - the live reading or IDN string.
    /// Thread-safe, non-blocking.
    void setValue(const char *text);

    /// Show the CSV file currently being logged into (basename; "" = none).
    /// Sent by the StorageTask on session start/stop and on rotation.
    /// Thread-safe, non-blocking.
    void setFile(const char *text);

    /// Re-render the menu if it is on screen. Called by the other tasks after
    /// they actually process a menu action (cycle/toggle/eject/mount), because
    /// those run asynchronously - rendering right after the button press would
    /// show the previous state. Thread-safe, non-blocking.
    void refreshMenu();

protected:
    void loop() override;

private:
    static constexpr const char *LOG_TAG = "DSPL";
    static constexpr size_t TEXT_MAX = 224; ///< fits a multi-line value block

    enum class Button : uint8_t
    {
        Menu,
        Up,
        Down,
        Ok,
    };

    enum class Screen : uint8_t
    {
        //Splash, 
        Live,  ///< startup screen now
        Menu,
    };

    // Menu items, in display order.
    enum class MenuItem : uint8_t
    {
        Interval,
        Quantity,
        Logging,
        SdInfo,
        Wifi,
        PcMode,
        COUNT,
    };

    struct Msg
    {
        enum class Kind : uint8_t
        {
            Status,
            Value,
            File,    ///< current CSV file name (Live, small line)
            Button,
            Refresh, ///< menu state changed in another task
        } kind;
        char text[TEXT_MAX]; ///< Status / Value / File
        bool highlight;      ///< Status: yellow (SD-loaded profile)
        Button button;       ///< Button
    };

    void postText(Msg::Kind kind, const char *text, bool highlight = false);
    bool buildUi();
    bool initButtons();

    void onButton(Button b);
    void activateItem();
    void showLive();
    void renderMenu();
    void enterPcMode();     ///< eject SD safely, then reboot as a USB disk
    void updateIcons();     ///< refresh the status icon bar (run / SD / USB / WiFi)
    void updateFileLine();  ///< bottom line: file name + session elapsed time

    // Button callbacks (run in the iot_button task context; they only queue).
    static void cbMenu(void *btn, void *arg);
    static void cbUp(void *btn, void *arg);
    static void cbDown(void *btn, void *arg);
    static void cbOk(void *btn, void *arg);

    QueueHandle_t _queue{nullptr};

    // LVGL objects - only ever touched from loop().
    void *_lblStatus{nullptr}; ///< lv_obj_t* status label (Live)
    void *_lblValue{nullptr};  ///< lv_obj_t* value label (Live)
    void *_lblFile{nullptr};   ///< lv_obj_t* current CSV name (Live, small)
    void *_lblHint{nullptr};   ///< lv_obj_t* button hint line (Live, bottom)
    void *_lblMenu{nullptr};   ///< lv_obj_t* menu block (Menu)
    // Status icon bar (top-right, visible on both screens).
    void *_icoRun{nullptr};    ///< lv_obj_t* measurement running / paused
    void *_icoSd{nullptr};     ///< lv_obj_t* SD card present / ejected
    void *_icoUsb{nullptr};    ///< lv_obj_t* instrument attached / PC mode
    void *_icoWifi{nullptr};   ///< lv_obj_t* Soft AP on / off

    // Menu is a thin view: it reads the InstrumentTask UiState snapshot and
    // triggers cycle/toggle actions on it - no local settings model here.
    Screen _screen{Screen::Live};
    uint8_t _menuSel{0};
    bool _fileVisible{false}; ///< logging: file name replaces the button hint
    char _fileName[64]{};     ///< current CSV basename (for the elapsed-time line)
};
