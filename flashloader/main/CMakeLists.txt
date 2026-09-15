idf_component_register(SRCS "main.c"
                            "fl_display.c"
                            "fl_power.c"
                            "fl_font.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES user_store driver esp_lcd esp_adc esp_wifi nvs_flash
                                     esp_event esp_netif esp_http_client esp_https_ota
                                     app_update mbedtls esp_timer heap
                                     esp_driver_spi esp_driver_ledc)
