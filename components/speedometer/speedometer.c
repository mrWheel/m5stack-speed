#include "speedometer.h"

#include <math.h>
#include <string.h>

void speedometer_init(speedometer_t *s, float initial_total_distance_m)
{
  memset(s, 0, sizeof(*s));
  s->total_distance_m = fmaxf(0.0f, initial_total_distance_m);
}

void speedometer_update(speedometer_t *s, const gps_data_t *gps, int64_t now_us)
{
  if (!s || !gps) return;

  s->gps_fix = gps->fix_valid;
  s->satellites = gps->satellites;
  s->raw_speed_kmh = gps->fix_valid ? fmaxf(0.0f, gps->speed_kmh) : 0.0f;

  float target = s->raw_speed_kmh;
  if (target < 0.8f) target = 0.0f;

  if (s->last_sample_us == 0)
  {
    s->display_speed_kmh = target;
    s->last_sample_us = gps->sample_time_us ? gps->sample_time_us : now_us;
    return;
  }

  float diff = fabsf(target - s->display_speed_kmh);
  float alpha = 0.25f;
  if (diff > 5.0f) alpha = 0.75f;
  else if (diff > 2.0f) alpha = 0.50f;

  s->display_speed_kmh += alpha * (target - s->display_speed_kmh);
  if (s->display_speed_kmh < 0.4f) s->display_speed_kmh = 0.0f;
  if (s->display_speed_kmh > 999.0f) s->display_speed_kmh = 999.0f;

  int64_t sample_us = gps->sample_time_us ? gps->sample_time_us : now_us;
  float dt = (float)(sample_us - s->last_sample_us) / 1000000.0f;
  s->last_sample_us = sample_us;

  if (dt <= 0.0f || dt > 2.0f || !gps->fix_valid)
  {
    return;
  }

  if (!s->trip_started && target >= 1.0f)
  {
    s->trip_started = true;
    s->trip_start_us = sample_us;
  }

  // Integrate GNSS speed rather than noisy point-to-point position changes.
  // Trapezoidal integration is intentionally avoided here because raw speed
  // from the previous sample is not retained separately; at 5-10 Hz the
  // midpoint error is negligible for this display application.
  if (target >= 1.0f)
  {
    float meters = (target / 3.6f) * dt;
    if (meters >= 0.0f && meters < 100.0f)
    {
      s->trip_distance_m += meters;
      s->total_distance_m += meters;
    }
  }
}

void speedometer_reset_trip(speedometer_t *s)
{
  if (!s) return;
  s->trip_distance_m = 0.0f;
  s->trip_started = false;
  s->trip_start_us = 0;
}

float speedometer_average_kmh(const speedometer_t *s)
{
  if (!s || !s->trip_started || s->trip_start_us == 0 || s->last_sample_us <= s->trip_start_us)
  {
    return 0.0f;
  }

  float hours = (float)(s->last_sample_us - s->trip_start_us) / 3600000000.0f;
  if (hours <= 0.0f) return 0.0f;
  float km = s->trip_distance_m / 1000.0f;
  float average = km / hours;
  if (average < 0.0f) average = 0.0f;
  if (average > 999.0f) average = 999.0f;
  return average;
}
