#include "gps.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "gps";
static gps_config_t s_cfg;
static gps_data_t s_latest;
static SemaphoreHandle_t s_lock;
static uint32_t s_last_read_sequence;

static bool checksum_ok(const char* line)
{
  if (!line || line[0] != '$')
    return false;
  const char* star = strchr(line, '*');
  if (!star || strlen(star) < 3)
    return false;

  uint8_t sum = 0;
  for (const char* p = line + 1; p < star; ++p)
    sum ^= (uint8_t)*p;

  char hex[3] = {star[1], star[2], 0};
  uint8_t expected = (uint8_t)strtoul(hex, NULL, 16);
  return sum == expected;
}

static int split_fields(char* line, char** fields, int max_fields)
{
  int count = 0;
  char* p = line;
  while (p && count < max_fields)
  {
    fields[count++] = p;
    char* comma = strchr(p, ',');
    if (!comma)
      break;
    *comma = 0;
    p = comma + 1;
  }
  return count;
}

static bool sentence_type(const char* field0, const char* type)
{
  size_t n = strlen(field0);
  return n >= 3 && strcmp(field0 + n - 3, type) == 0;
}

static double parse_coordinate(const char* value, const char* hemisphere)
{
  if (!value || !value[0] || !hemisphere || !hemisphere[0])
    return 0.0;

  double raw = strtod(value, NULL);
  double degrees = (double)((int)(raw / 100.0));
  double coordinate = degrees + (raw - degrees * 100.0) / 60.0;
  if (hemisphere[0] == 'S' || hemisphere[0] == 'W')
    coordinate = -coordinate;
  return coordinate;
}

static void publish_rmc(char* line)
{
  char* star = strchr(line, '*');
  if (star)
    *star = 0;

  char* fields[20] = {0};
  int count = split_fields(line, fields, 20);
  if (count < 9 || !sentence_type(fields[0], "RMC"))
    return;

  bool valid = fields[2][0] == 'A';
  float knots = fields[7][0] ? strtof(fields[7], NULL) : 0.0f;
  float course = fields[8][0] ? strtof(fields[8], NULL) : 0.0f;
  double latitude = parse_coordinate(fields[3], fields[4]);
  double longitude = parse_coordinate(fields[5], fields[6]);

  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint8_t day = 0;
  uint8_t month = 0;
  uint16_t year = 0;
  bool date_valid = false;

  if (fields[1][0])
  {
    char time_str[7] = {0};
    unsigned long time_value;
    memcpy(time_str, fields[1], 6);
    time_value = strtoul(time_str, NULL, 10);
    hour = (uint8_t)(time_value / 10000);
    minute = (uint8_t)((time_value / 100) % 100);
    second = (uint8_t)(time_value % 100);
  }

  if (fields[9][0])
  {
    char date_str[7] = {0};
    unsigned long date_value;
    memcpy(date_str, fields[9], 6);
    date_value = strtoul(date_str, NULL, 10);
    day = (uint8_t)(date_value / 10000);
    month = (uint8_t)((date_value / 100) % 100);
    year = (uint16_t)(2000 + (date_value % 100));
    date_valid = day > 0 && day <= 31 && month > 0 && month <= 12 && year >= 2000;
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_latest.fix_valid = valid;
  s_latest.speed_kmh = valid ? knots * 1.852f : 0.0f;
  s_latest.course_deg = course;
  s_latest.latitude_deg = latitude;
  s_latest.longitude_deg = longitude;
  s_latest.day = day;
  s_latest.month = month;
  s_latest.year = year;
  s_latest.hour = hour;
  s_latest.minute = minute;
  s_latest.second = second;
  s_latest.date_valid = date_valid;
  s_latest.sample_time_us = esp_timer_get_time();
  s_latest.sequence++;
  xSemaphoreGive(s_lock);
}

static void publish_gga(char* line)
{
  char* star = strchr(line, '*');
  if (star)
    *star = 0;

  char* fields[20] = {0};
  int count = split_fields(line, fields, 20);
  if (count < 8 || !sentence_type(fields[0], "GGA"))
    return;

  int quality = fields[6][0] ? atoi(fields[6]) : 0;
  int sats = fields[7][0] ? atoi(fields[7]) : 0;
  float altitude = fields[9] && fields[9][0] ? strtof(fields[9], NULL) : 0.0f;
  if (sats < 0)
    sats = 0;
  if (sats > 99)
    sats = 99;

  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_latest.satellites = (uint8_t)sats;
  s_latest.altitude_m = altitude;
  if (quality > 0)
    s_latest.fix_valid = true;
  xSemaphoreGive(s_lock);
}

static void process_line(char* line)
{
  if (!checksum_ok(line))
    return;
  if (strstr(line, "RMC,"))
    publish_rmc(line);
  else if (strstr(line, "GGA,"))
    publish_gga(line);
}

static void gps_task(void* arg)
{
  (void)arg;
  uint8_t buf[256];
  char line[192];
  size_t line_len = 0;

  if (s_cfg.request_10hz)
  {
    // CASIC PCAS02: request 100 ms positioning interval = 10 Hz.
    // Sent on every boot; no flash-save command is used.
    const char* cmd = "$PCAS02,100*1E\r\n";
    for (int i = 0; i < 3; ++i)
    {
      uart_write_bytes(s_cfg.uart_num, cmd, strlen(cmd));
      uart_wait_tx_done(s_cfg.uart_num, pdMS_TO_TICKS(100));
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }

  while (true)
  {
    int n = uart_read_bytes(s_cfg.uart_num, buf, sizeof(buf), pdMS_TO_TICKS(100));
    for (int i = 0; i < n; ++i)
    {
      char c = (char)buf[i];
      if (c == '$')
      {
        line_len = 0;
        line[line_len++] = c;
      }
      else if (c == '\n')
      {
        if (line_len > 6)
        {
          line[line_len] = 0;
          process_line(line);
        }
        line_len = 0;
      }
      else if (c != '\r' && line_len > 0 && line_len < sizeof(line) - 1)
      {
        line[line_len++] = c;
      }
    }
  }
}

esp_err_t gps_init(const gps_config_t* config)
{
  if (!config)
    return ESP_ERR_INVALID_ARG;
  s_cfg = *config;
  memset(&s_latest, 0, sizeof(s_latest));
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock)
    return ESP_ERR_NO_MEM;

  uart_config_t uart_cfg = {
      .baud_rate = config->baud_rate,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  ESP_ERROR_CHECK(uart_driver_install(config->uart_num, 4096, 0, 0, NULL, 0));
  ESP_ERROR_CHECK(uart_param_config(config->uart_num, &uart_cfg));
  ESP_ERROR_CHECK(uart_set_pin(config->uart_num, config->tx_gpio, config->rx_gpio,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

  xTaskCreate(gps_task, "gps", 4096, NULL, 10, NULL);
  ESP_LOGI(TAG, "AT6668 UART ready at %d baud", config->baud_rate);
  return ESP_OK;
}

bool gps_get_latest(gps_data_t* out)
{
  if (!out || !s_lock)
    return false;
  bool changed = false;

  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_latest.sequence != s_last_read_sequence)
  {
    *out = s_latest;
    s_last_read_sequence = s_latest.sequence;
    changed = true;
  }
  xSemaphoreGive(s_lock);
  return changed;
}
