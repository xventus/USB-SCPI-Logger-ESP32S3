//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   literals.h
/// @author Petr Vanek

#pragma once

#include <stdio.h>

class literals
{
public:
    // internal - do not modify

    // FreeRTOS task names
    static constexpr const char *tsk_display{"DSPLTSK"};
    static constexpr const char *tsk_storage{"STORTSK"};
    static constexpr const char *tsk_instrument{"INSTTSK"};
    static constexpr const char *tsk_wifi{"WIFITSK"};

    // ------------------------------------------------------------------
    // Display texts (everything the user sees on the LCD; log messages
    // stay inline at their call sites)
    // Texts for translation here !!!!
    // ------------------------------------------------------------------

    // Live screen
    static constexpr const char *ui_title{"SCPI Logger"};
    static constexpr const char *ui_starting{"Starting..."};
    static constexpr const char *ui_hint_live{"OK run  UP pc  DN wifi  MENU menu"};
    static constexpr const char *ui_waiting{"Waiting for instrument"};
    static constexpr const char *ui_usb_host_failed{"USB host init FAILED"};
    static constexpr const char *ui_not_usbtmc{"Connected (not USBTMC)"};
    static constexpr const char *ui_reading_idn{"Reading *IDN? ..."};
    static constexpr const char *ui_redetecting{"Re-detecting..."};
    static constexpr const char *ui_idn_failed{"IDN failed - replug"};
    static constexpr const char *ui_no_profile{"No profile"};
    static constexpr const char *ui_paused{"Paused"};
    static constexpr const char *ui_sd_busy{"SD busy - try again"};

    // WiFi Soft AP
    static constexpr const char *fmt_ap_on{"AP %s\nhttp://%s"};
    static constexpr const char *ui_ap_off{"WiFi AP off"};

    // PC mode (USB MSC disk)
    static constexpr const char *ui_pc_mode{"PC mode - USB disk"};
    static constexpr const char *ui_pc_mode_failed{"PC mode FAILED (no SD?)"};
    static constexpr const char *ui_pc_back{"UP = back to logger"};

    // Menu screen (LVGL recolor syntax: "#rrggbb text#"; selected line and
    // its chevron are green, the copyright footer blue)
    static constexpr const char *ui_menu_title{"MENU"};
    static constexpr const char *fmt_menu_item_sel{"#33ff66 > %s#\n"};
    static constexpr const char *fmt_menu_item{"   %s\n"};
    static constexpr const char *fmt_menu_interval{"Interval: %s"};
    static constexpr const char *fmt_menu_quantity{"Quantity: %s"};
    static constexpr const char *fmt_menu_logging{"Logging:  %s"};
    static constexpr const char *fmt_menu_sd{"SD: %s"};
    static constexpr const char *fmt_menu_wifi{"WiFi AP:  %s"};
    static constexpr const char *ui_menu_pcmode{"PC mode (USB disk)"};
    static constexpr const char *ui_menu_footer{"\n#00aaff (c)2026 fotoventus.cz#"};
    static constexpr const char *ui_on{"ON"};
    static constexpr const char *ui_off{"OFF"};
    static constexpr const char *ui_none{"-"};
    static constexpr const char *ui_sd_ejected{"ejected (OK=mount)"};

    // Display value/format strings
    static constexpr const char *fmt_usb_vidpid{"USB %04X:%04X"};
    static constexpr const char *fmt_value{"%.6f %s"};             ///< select_single reading
    static constexpr const char *fmt_value_line{"%s%s %.4f %s"};   ///< one line of the multi-channel block
    static constexpr const char *fmt_interval_s{"%lu s"};
    static constexpr const char *fmt_interval_frac{"%.1f s"};
    static constexpr const char *fmt_sd_info{"%.1f GB (OK=eject)"}; ///< free space
    static constexpr const char *fmt_file_time_h{"%s  %u:%02u:%02u"}; ///< file + h:mm:ss
    static constexpr const char *fmt_file_time_m{"%s  %u:%02u"};      ///< file + m:ss
};
