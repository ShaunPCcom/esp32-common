if(NOT "${IDF_TARGET}" STREQUAL "esp32c6")
    idf_component_register(INCLUDE_DIRS "include")
    return()
endif()

idf_component_register(
    SRCS "src/web_server_base.c"
         "src/web_server_base_sse.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_wifi esp_netif nvs_flash spiffs json
             esp_timer freertos esp_system lwip
    PRIV_REQUIRES ota_check wifi_manager zigbee_ota crash_diag
)
