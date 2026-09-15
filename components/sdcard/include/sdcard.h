#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gps.h"

//-- Mount point used by the SD card FatFs volume, shared with the webserver file manager.
#define SDCARD_MOUNT_POINT "/sdcard"

typedef struct
{
  bool mounted;
  uint64_t total_bytes;
  uint64_t free_bytes;
  uint8_t free_percent;
} sdcard_status_t;

esp_err_t sdcard_init(void);
esp_err_t sdcard_reset_trip(void);
esp_err_t sdcard_format(void);
esp_err_t sdcard_remove_small_trip_files(void);
esp_err_t sdcard_append_fix(const gps_data_t* gps, float trip_distance_m);
uint32_t sdcard_get_entry_count(void);
void sdcard_get_status(sdcard_status_t* status);
uint16_t sdcard_get_trip_number(void);
esp_err_t sdcard_finish(void);
