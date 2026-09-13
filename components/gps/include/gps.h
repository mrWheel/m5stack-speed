#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

typedef struct
{
  uart_port_t uart_num;
  int rx_gpio;
  int tx_gpio;
  int baud_rate;
  bool request_10hz;
} gps_config_t;

typedef struct
{
  bool fix_valid;
  float speed_kmh;
  float course_deg;
  uint8_t satellites;
  int64_t sample_time_us;
  uint32_t sequence;
} gps_data_t;

esp_err_t gps_init(const gps_config_t *config);
bool gps_get_latest(gps_data_t *out);
