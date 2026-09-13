#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "gps.h"

typedef struct
{
  float raw_speed_kmh;
  float display_speed_kmh;
  float trip_distance_m;
  float total_distance_m;
  bool gps_fix;
  uint8_t satellites;

  bool trip_started;
  int64_t trip_start_us;
  int64_t last_sample_us;
} speedometer_t;

void speedometer_init(speedometer_t *s, float initial_total_distance_m);
void speedometer_update(speedometer_t *s, const gps_data_t *gps, int64_t now_us);
void speedometer_reset_trip(speedometer_t *s);
float speedometer_average_kmh(const speedometer_t *s);
