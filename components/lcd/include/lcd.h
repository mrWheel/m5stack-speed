#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define LCD_COLOR_BLACK 0x0000
#define LCD_COLOR_WHITE 0xFFFF
#define LCD_COLOR_GREEN 0x07E0
#define LCD_COLOR_YELLOW 0xFFE0
#define LCD_COLOR_RED 0xF800
#define LCD_COLOR_CYAN 0x07FF
#define LCD_COLOR_DARKGREY 0x4208

typedef struct
{
  float speed_kmh;
  bool average_mode;
  float distance_m;
  bool total_mode;
  bool gps_fix;
  uint8_t satellites;
  int battery_pct;
  bool charging;
} lcd_view_t;

esp_err_t lcd_init(void);
void lcd_set_backlight(bool on);
void lcd_clear(uint16_t color);
void lcd_render(const lcd_view_t *view);
void lcd_force_redraw(void);
