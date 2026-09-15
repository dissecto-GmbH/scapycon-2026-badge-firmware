get_filename_component(BADGE_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(WIFI_CONF "${BADGE_PROJECT_DIR}/config/wifi.conf")
if(NOT EXISTS "${WIFI_CONF}")
    message(FATAL_ERROR
        "Missing config/wifi.conf. Copy config/wifi.conf.example to config/wifi.conf and set credentials.")
endif()

file(READ "${WIFI_CONF}" WIFI_CONF_RAW)
string(REPLACE "\r\n" "\n" WIFI_CONF_RAW "${WIFI_CONF_RAW}")
string(REPLACE "\r" "\n" WIFI_CONF_RAW "${WIFI_CONF_RAW}")
string(REPLACE "\n" ";" WIFI_CONF_LINES "${WIFI_CONF_RAW}")

set(WIFI_SSID "")
set(WIFI_PASSWORD "")
foreach(LINE IN LISTS WIFI_CONF_LINES)
    string(STRIP "${LINE}" LINE)
    if(LINE MATCHES "^#")
        continue()
    endif()
    if(LINE MATCHES "^ssid=(.+)$")
        set(WIFI_SSID "${CMAKE_MATCH_1}")
    elseif(LINE MATCHES "^password=(.+)$")
        set(WIFI_PASSWORD "${CMAKE_MATCH_1}")
    endif()
endforeach()

if(WIFI_SSID STREQUAL "" OR WIFI_PASSWORD STREQUAL "")
    message(FATAL_ERROR "config/wifi.conf must contain ssid= and password= lines")
endif()

set(WIFI_CREDS_H "${CMAKE_CURRENT_BINARY_DIR}/wifi_credentials.h")
file(WRITE "${WIFI_CREDS_H}"
"#pragma once\n"
"#define WIFI_SSID \"${WIFI_SSID}\"\n"
"#define WIFI_PASSWORD \"${WIFI_PASSWORD}\"\n"
)

set(BADGE_CONF "${BADGE_PROJECT_DIR}/config/badge.conf")
if(NOT EXISTS "${BADGE_CONF}")
    message(FATAL_ERROR
        "Missing config/badge.conf. Copy config/badge.conf.example to config/badge.conf and set name=.")
endif()

file(READ "${BADGE_CONF}" BADGE_CONF_RAW)
string(REPLACE "\r\n" "\n" BADGE_CONF_RAW "${BADGE_CONF_RAW}")
string(REPLACE "\r" "\n" BADGE_CONF_RAW "${BADGE_CONF_RAW}")
string(REPLACE "\n" ";" BADGE_CONF_LINES "${BADGE_CONF_RAW}")

set(BADGE_DISPLAY_NAME "")
foreach(LINE IN LISTS BADGE_CONF_LINES)
    string(STRIP "${LINE}" LINE)
    if(LINE MATCHES "^#")
        continue()
    endif()
    if(LINE MATCHES "^name=(.+)$")
        set(BADGE_DISPLAY_NAME "${CMAKE_MATCH_1}")
    endif()
endforeach()

if(BADGE_DISPLAY_NAME STREQUAL "")
    message(FATAL_ERROR "config/badge.conf must contain a name= line")
endif()

set(BADGE_CONFIG_H "${CMAKE_CURRENT_BINARY_DIR}/badge_config.h")
file(WRITE "${BADGE_CONFIG_H}"
"#pragma once\n"
"#define BADGE_DISPLAY_NAME \"${BADGE_DISPLAY_NAME}\"\n"
)

idf_component_register(SRCS "main.c"
                            "ui.c"
                            "display.c"
                            "font8x8_basic.c"
                            "leds.c"
                            "led_strip_encoder.c"
                            "its_g5.c"
                            "wifi.c"
                            "radio.c"
                            "host_link.c"
                            "button.c"
                            "badge_name.c"
                            "badge_setup.c"
                            "power.c"
                            "utf8_name.c"
                            "usb_mode_cmd.c"
                       INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
                       PRIV_REQUIRES user_store driver esp_lcd esp_driver_rmt heap
                                     esp_wifi nvs_flash esp_event esp_netif
                                     esp_driver_usb_serial_jtag esp_timer
                                     esp_driver_spi esp_driver_ledc esp_adc
                                     esp_pm esp_hw_support vfs app_update)

# Undocumented PHY symbols live in libphy.a — force the linker to resolve them.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,phy_11p_set"
    "-Wl,-u,phy_change_channel"
)
