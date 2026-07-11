# C++ source files of the application
set(MAIN_SOURCES
    "main.cpp"             # application entry point
    "application.cpp"      # application singleton, task orchestration
    "rptask.cpp"           # FreeRTOS task base class
    "display_task.cpp"     # ST7789 LVGL UI task
    "storage_task.cpp"     # SD card / CSV writer task
    "instrument_task.cpp"  # USB host + USBTMC instrument task
    "csv_logger.cpp"       # CSV writer to the SD card
    "profile.cpp"          # instrument profiles (built-in + JSON from SD)
    "usb_mode.cpp"         # logger / PC-disk boot mode selection
    "msc_device.cpp"       # PC mode: SD card as a USB MSC disk (TinyUSB)
    "wifi_task.cpp"        # on-demand WiFi Soft AP
    "web_server.cpp"       # HTTP server: static page + REST API
)

# Register the component with the ESP-IDF build system.
# usb_host_* API comes from the "usb" component; the BSP (and LVGL) are pulled
# in as managed components via idf_component.yml.
idf_component_register(
    SRCS ${MAIN_SOURCES}
    INCLUDE_DIRS "."
    # button: board buttons (iot_button) for the menu; fatfs: esp_vfs_fat_info
    # for SD free-space in the menu; esp_driver_sdmmc + sdmmc: raw SD card init
    # for the MSC PC mode. The BSP, LVGL and esp_tinyusb come via idf_component.yml.
    REQUIRES driver usb usbtmc button fatfs esp_timer json esp_driver_sdmmc sdmmc
             esp_wifi esp_netif nvs_flash esp_http_server
)

# Compile C++ sources as C++20 (same as the reference project)
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++20>
)

# Build the LittleFS image for the "web" partition from web/ and flash it
# together with the app (idf.py flash).
littlefs_create_partition_image(web ../web FLASH_IN_PROJECT)
