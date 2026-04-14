idf_component_register(
    SRCS "src/zigbee_core.cpp"
         "src/zigbee_button.cpp"
         "src/zgp_stub.c"
         "src/zigbee_ctrl.c"
         "src/zigbee_signal_handler.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp-zigbee-lib nvs_flash esp_driver_gpio esp_system
)
