idf_component_register(
    SRCS "src/crash_diag.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_system heap
)
