idf_component_register(
    SRCS "src/web_server_base.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_wifi esp_netif nvs_flash spiffs cJSON
             esp_timer freertos esp_system lwip
    PRIV_REQUIRES ota_check wifi_manager zigbee_ota crash_diag
)
