set(PC_REQUIRES espressif__esp-zigbee-lib nvs_flash esp_timer)

if("${IDF_TARGET}" STREQUAL "esp32c6")
    list(APPEND PC_REQUIRES web_server_base esp_http_server)
endif()

idf_component_register(
    SRCS "src/portable_config.c"
         "src/portable_config_zigbee.c"
         "src/portable_config_http.c"
    INCLUDE_DIRS "include"
    REQUIRES ${PC_REQUIRES}
)
