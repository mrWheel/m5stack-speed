idf_component_register(
  SRCS "app_main.c"
  INCLUDE_DIRS "."
  REQUIRES board gps lcd speedometer nvs_flash
)
