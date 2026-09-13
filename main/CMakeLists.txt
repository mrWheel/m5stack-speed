idf_component_register(
  SRCS "app_main.c"
  INCLUDE_DIRS "."
  REQUIRES board gps lcd sdcard speedometer webserver nvs_flash esp_hw_support
)
