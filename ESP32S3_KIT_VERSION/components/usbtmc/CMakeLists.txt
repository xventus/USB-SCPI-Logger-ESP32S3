# USBTMC transport component.
#
# A hand-written USBTMC bulk transport on top of the generic usb_host client
# API (ESP-IDF has no TMC class driver). This is the core of the product: it
# knows nothing about profiles, CSV or the UI - it just claims the USBTMC
# interface of an already-enumerated device and runs SCPI query/write
# transactions.
idf_component_register(
    SRCS "usbtmc.cpp"
    INCLUDE_DIRS "include"
    REQUIRES usb esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++20>
)
