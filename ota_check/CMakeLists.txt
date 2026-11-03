if(NOT "${IDF_TARGET}" STREQUAL "esp32c6")
    idf_component_register(INCLUDE_DIRS "include")
    return()
endif()

idf_component_register(
    SRCS "src/ota_check.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_http_client mbedtls freertos zigbee_ota
)
