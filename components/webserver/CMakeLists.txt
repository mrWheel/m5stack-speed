idf_component_register(
  SRCS "webserver.c" "webserver_api.c" "webserver_static.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_http_server sdcard joltwallet__littlefs michmich__esp-idf-wifi-provisioner esp_wifi esp_netif espressif__cjson espressif__mdns
)

#-- The GUI assets (index.html, style.css, app.js) are flashed as a LittleFS image
#-- into the "littlefs" partition declared in the project's partitions.csv.
littlefs_create_partition_image(littlefs gui FLASH_IN_PROJECT)
