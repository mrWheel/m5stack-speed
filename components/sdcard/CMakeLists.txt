idf_component_register(
  SRCS "sdcard.c"
  INCLUDE_DIRS "include"
  REQUIRES fatfs sdmmc esp_driver_spi gps nvs_flash
)
