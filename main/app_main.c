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
static bool g_system_menu = false;
static uint8_t g_menu_selection = 0;
static bool g_menu_action_active = false;
static bool g_menu_action_pending = false;
static uint8_t g_menu_action_selection = 0;
static int64_t g_menu_action_requested_us = 0;
static int64_t g_last_user_activity_us = 0;
static uint32_t g_last_total_save_m = 0;
static sdcard_status_t g_storage_status;

static const char *menu_option_name(uint8_t selection)
{
  switch (selection)
  {
    case 0: return "Reset Trip";
    case 1: return "Used Free";
    case 2: return "Format SD";
    case 3: return "Exit";
    default: return "Unknown";
  }
}

static void log_menu_cursor(board_button_t button)
{
  ESP_LOGI("board", "Button %s => [%s]",
           button == BOARD_BUTTON_A ? "A (LEFT)" : "C (RIGHT)",
           menu_option_name(g_menu_selection));
}

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

static void reset_trip(speedometer_t *speedo)
{
  speedometer_reset_trip(speedo);
  g_show_total = false;
  if (sdcard_reset_trip() != ESP_OK)
  {
    ESP_LOGE(TAG, "Unable to create the next trip export");
  }
}

static void format_sdcard(void)
{
  ESP_LOGI("board", "Button B (MIDDLE) => [Formatting SD]");
  if (sdcard_format() != ESP_OK)
  {
    ESP_LOGE(TAG, "Unable to format the SD card");
  }
  sdcard_get_status(&g_storage_status);
}

static void handle_button(board_button_t button, bool long_press, speedometer_t *speedo)
{
  if (g_menu_action_active)
  {
    if (button == BOARD_BUTTON_B && !long_press)
    {
      g_menu_action_active = false;
      g_menu_action_pending = false;
      g_system_menu = true;
      lcd_force_redraw();
      ESP_LOGI("board", "System Menu => [%s]", menu_option_name(g_menu_selection));
    }
    return;
  }

  if (button == BOARD_BUTTON_B && long_press)
  {
    g_system_menu = !g_system_menu;
    g_menu_selection = 0;
    if (g_system_menu)
    {
      turn_display_on();
      ESP_LOGI("board", "System Menu => [%s]", menu_option_name(g_menu_selection));
    }
    else
    {
      ESP_LOGI("board", "System Menu => [Closed]");
    }
    lcd_force_redraw();
    return;
  }

  if (!g_display_on)
  {
    turn_display_on();
    g_display_forced_off = false;
    return;
  }

  g_last_user_activity_us = esp_timer_get_time();

  if (g_system_menu)
  {
    switch (button)
    {
      case BOARD_BUTTON_A:
        if (!long_press && g_menu_selection > 0)
        {
          --g_menu_selection;
          log_menu_cursor(button);
        }
        break;
      case BOARD_BUTTON_B:
        if (!long_press)
        {
          ESP_LOGI("board", "Button B (MIDDLE) => [%s]",
                   menu_option_name(g_menu_selection));
          g_menu_action_active = true;
          g_menu_action_pending = true;
          g_menu_action_selection = g_menu_selection;
          g_menu_action_requested_us = esp_timer_get_time();
          g_system_menu = false;
          lcd_force_redraw();
        }
        break;
      case BOARD_BUTTON_C:
        if (!long_press && g_menu_selection < 3)
        {
          ++g_menu_selection;
          log_menu_cursor(button);
        }
        break;
      default:
        break;
    }
    return;
  }

  switch (button)
  {
    case BOARD_BUTTON_A:
      if (long_press)
      {
        reset_trip(speedo);
      }
      else
      {
        g_show_total = !g_show_total;
      }
      break;

    case BOARD_BUTTON_B:
      if (g_display_on)
      {
        turn_display_off(true);
      }
      else
      {
        turn_display_on();
        g_display_forced_off = false;
      }
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

    if (g_menu_action_active && g_menu_action_pending &&
        now_us - g_menu_action_requested_us >= 100000LL)
    {
      g_menu_action_pending = false;
      if (g_menu_action_selection == 0)
      {
        reset_trip(&speedo);
      }
      else if (g_menu_action_selection == 1)
      {
        sdcard_get_status(&g_storage_status);
      }
      else if (g_menu_action_selection == 2)
      {
        format_sdcard();
        g_menu_action_active = false;
        g_menu_action_pending = false;
        g_system_menu = true;
        lcd_force_redraw();
        ESP_LOGI("board", "System Menu => [%s]", menu_option_name(g_menu_selection));
      }
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
        .storage_total_bytes = g_storage_status.total_bytes,
        .storage_free_bytes = g_storage_status.free_bytes,
        .system_menu = g_system_menu,
        .menu_selection = g_menu_selection,
        .menu_action = g_menu_action_active,
        .action_selection = g_menu_action_selection,
      };
      lcd_render(&view);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
