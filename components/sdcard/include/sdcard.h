#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gps.h"

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
esp_err_t sdcard_append_fix(const gps_data_t *gps);
void sdcard_get_status(sdcard_status_t *status);
esp_err_t sdcard_finish(void);
