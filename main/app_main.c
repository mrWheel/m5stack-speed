#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "board.h"
#include "gps.h"
#include "lcd.h"
#include "sdcard.h"
#include "speedometer.h"

static const char *TAG = "m5speed";

static bool g_show_average = false;
static bool g_show_total = false;
static bool g_display_on = true;
static bool g_display_forced_off = false;
static int64_t g_last_user_activity_us = 0;
static uint32_t g_last_total_save_m = 0;
static sdcard_status_t g_storage_status;

static float load_total_distance_m(void)
{
  nvs_handle_t nvs;
  uint32_t cm = 0;
  if (nvs_open("speed", NVS_READONLY, &nvs) == ESP_OK)
  {
    nvs_get_u32(nvs, "total_cm", &cm);
    nvs_close(nvs);
  }
  return (float)cm / 100.0f;
}

static void save_total_distance_m(float meters)
{
  nvs_handle_t nvs;
  if (nvs_open("speed", NVS_READWRITE, &nvs) != ESP_OK)
  {
    return;
  }

  uint32_t cm = (uint32_t)lroundf(fmaxf(0.0f, meters) * 100.0f);
  if (nvs_set_u32(nvs, "total_cm", cm) == ESP_OK)
  {
    nvs_commit(nvs);
  }
  nvs_close(nvs);
}

static uint32_t timeout_for_battery(int battery_pct, bool charging)
{
  if (charging)
  {
    return 0; // no automatic timeout while charging/USB powered
  }
  if (battery_pct >= 100) return 120;
  if (battery_pct >= 75)  return 90;
  if (battery_pct >= 50)  return 60;
  if (battery_pct >= 25)  return 30;
  return 15;
}

static void turn_display_on(void)
{
  if (!g_display_on)
  {
    lcd_set_backlight(true);
    g_display_on = true;
    lcd_force_redraw();
  }
  g_last_user_activity_us = esp_timer_get_time();
}

static void turn_display_off(bool forced)
{
  lcd_set_backlight(false);
  g_display_on = false;
  g_display_forced_off = forced;
}

static void handle_button(board_button_t button, bool long_press, speedometer_t *speedo)
{
  if (!g_display_on)
  {
    turn_display_on();
    g_display_forced_off = false;
    return;
  }

  g_last_user_activity_us = esp_timer_get_time();

  switch (button)
  {
    case BOARD_BUTTON_A:
      if (long_press)
      {
        speedometer_reset_trip(speedo);
        g_show_total = false;
        if (sdcard_reset_trip() != ESP_OK)
        {
          ESP_LOGE(TAG, "Unable to create the next trip export");
        }
      }
      else
      {
        g_show_total = !g_show_total;
      }
      break;

    case BOARD_BUTTON_B:
      turn_display_off(true);
      break;

    case BOARD_BUTTON_C:
      if (!long_press)
      {
        g_show_average = !g_show_average;
      }
      break;

    default:
      break;
  }
}

void app_main(void)
{
  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  else
  {
    ESP_ERROR_CHECK(nvs_err);
  }

  ESP_ERROR_CHECK(board_init());
  ESP_ERROR_CHECK(lcd_init());

  esp_err_t sdcard_err = sdcard_init();
  if (sdcard_err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card unavailable: %s", esp_err_to_name(sdcard_err));
  }

  lcd_clear(LCD_COLOR_BLACK);
  lcd_set_backlight(true);

  gps_config_t gps_cfg = {
    .uart_num = UART_NUM_2,
    .rx_gpio = 16,
    .tx_gpio = 17,
    .baud_rate = 115200,
    .request_10hz = true,
  };
  ESP_ERROR_CHECK(gps_init(&gps_cfg));

  speedometer_t speedo;
  float stored_total_m = load_total_distance_m();
  speedometer_init(&speedo, stored_total_m);
  g_last_total_save_m = (uint32_t)stored_total_m;

  g_last_user_activity_us = esp_timer_get_time();
  int64_t last_ui_us = 0;
  int64_t last_battery_us = 0;
  int battery_pct = board_battery_level();
  bool charging = board_is_charging();
  sdcard_get_status(&g_storage_status);

  ESP_LOGI(TAG, "M5Stack Speed started");
  ESP_LOGI(TAG, "GPS UART: RX=GPIO16, TX=GPIO17, 115200 8N1");

  while (true)
  {
    const int64_t now_us = esp_timer_get_time();

    board_button_event_t event;
    while (board_get_button_event(&event))
    {
      handle_button(event.button, event.long_press, &speedo);
    }

    gps_data_t gps;
    if (gps_get_latest(&gps))
    {
      speedometer_update(&speedo, &gps, now_us);
      if (sdcard_append_fix(&gps) != ESP_OK && g_storage_status.mounted)
      {
        ESP_LOGE(TAG, "Unable to append GPS fix to SD card");
      }
    }

    if ((now_us - last_battery_us) >= 2000000LL)
    {
      last_battery_us = now_us;
      battery_pct = board_battery_level();
      charging = board_is_charging();
      sdcard_get_status(&g_storage_status);
    }

    if (g_display_on && !g_display_forced_off)
    {
      uint32_t timeout_s = timeout_for_battery(battery_pct, charging);
      if (timeout_s > 0 && (now_us - g_last_user_activity_us) > (int64_t)timeout_s * 1000000LL)
      {
        turn_display_off(false);
      }
    }

    uint32_t total_m_whole = (uint32_t)(speedo.total_distance_m);
    if (total_m_whole >= g_last_total_save_m + 1000)
    {
      save_total_distance_m(speedo.total_distance_m);
      g_last_total_save_m = total_m_whole;
    }

    if (g_display_on && (now_us - last_ui_us) >= 50000LL)
    {
      last_ui_us = now_us;

      lcd_view_t view = {
        .speed_kmh = g_show_average ? speedometer_average_kmh(&speedo) : speedo.display_speed_kmh,
        .average_mode = g_show_average,
        .distance_m = g_show_total ? speedo.total_distance_m : speedo.trip_distance_m,
        .total_mode = g_show_total,
        .gps_fix = speedo.gps_fix,
        .satellites = speedo.satellites,
        .battery_pct = battery_pct,
        .charging = charging,
        .storage_available = g_storage_status.mounted,
        .storage_free_percent = g_storage_status.free_percent,
      };
      lcd_render(&view);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
