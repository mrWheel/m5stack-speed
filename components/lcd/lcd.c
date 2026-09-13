#include "lcd.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_HOST SPI3_HOST
#define LCD_MOSI GPIO_NUM_23
#define LCD_MISO GPIO_NUM_19
#define LCD_SCLK GPIO_NUM_18
#define LCD_CS   GPIO_NUM_14
#define LCD_DC   GPIO_NUM_27
#define LCD_RST  GPIO_NUM_33
#define LCD_BL   GPIO_NUM_32
#define LCD_W 320
#define LCD_H 240

static const char *TAG = "lcd";
static spi_device_handle_t s_spi;
static bool s_force_redraw = true;
static lcd_view_t s_prev;
static bool s_have_prev = false;

static void tx(bool data, const void *bytes, size_t len)
{
  if (!len) return;
  gpio_set_level(LCD_DC, data ? 1 : 0);
  spi_transaction_t t = {0};
  t.length = len * 8;
  t.tx_buffer = bytes;
  ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void cmd(uint8_t c)
{
  tx(false, &c, 1);
}

static void data8(uint8_t d)
{
  tx(true, &d, 1);
}

static void data(const uint8_t *d, size_t n)
{
  tx(true, d, n);
}

static void set_window(int x, int y, int w, int h)
{
  uint8_t d[4];
  cmd(0x2A);
  d[0] = (uint8_t)(x >> 8); d[1] = (uint8_t)x;
  d[2] = (uint8_t)((x + w - 1) >> 8); d[3] = (uint8_t)(x + w - 1);
  data(d, 4);
  cmd(0x2B);
  d[0] = (uint8_t)(y >> 8); d[1] = (uint8_t)y;
  d[2] = (uint8_t)((y + h - 1) >> 8); d[3] = (uint8_t)(y + h - 1);
  data(d, 4);
  cmd(0x2C);
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > LCD_W) w = LCD_W - x;
  if (y + h > LCD_H) h = LCD_H - y;
  if (w <= 0 || h <= 0) return;

  set_window(x, y, w, h);
  uint8_t block[256];
  for (size_t i = 0; i < sizeof(block); i += 2)
  {
    block[i] = (uint8_t)(color >> 8);
    block[i + 1] = (uint8_t)color;
  }

  int pixels = w * h;
  gpio_set_level(LCD_DC, 1);
  while (pixels > 0)
  {
    int n = pixels > 128 ? 128 : pixels;
    spi_transaction_t t = {0};
    t.length = n * 16;
    t.tx_buffer = block;
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
    pixels -= n;
  }
}

void lcd_clear(uint16_t color)
{
  fill_rect(0, 0, LCD_W, LCD_H, color);
}

static const uint8_t *glyph(char c)
{
  // 5-bit rows, seven rows per glyph. Only characters used by this UI.
  static const uint8_t blank[7] = {0,0,0,0,0,0,0};
  static const uint8_t digits[10][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
  };
  static const uint8_t letters[26][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,14}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,2,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
  };
  static const uint8_t dot[7] = {0,0,0,0,0,12,12};
  static const uint8_t slash[7] = {1,2,2,4,8,8,16};
  static const uint8_t percent[7] = {17,2,4,8,16,0,17};
  static const uint8_t dash[7] = {0,0,0,31,0,0,0};
  static const uint8_t colon[7] = {0,4,4,0,4,4,0};
  static const uint8_t plus[7] = {0,4,4,31,4,4,0};

  if (c >= '0' && c <= '9') return digits[c - '0'];
  if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
  if (c >= 'a' && c <= 'z') return letters[c - 'a'];
  if (c == '.') return dot;
  if (c == '/') return slash;
  if (c == '%') return percent;
  if (c == '-') return dash;
  if (c == ':') return colon;
  if (c == '+') return plus;
  return blank;
}

static int text_width(const char *s, int scale)
{
  return (int)strlen(s) * 6 * scale - scale;
}

static void draw_char(int x, int y, char c, int scale, uint16_t color)
{
  const uint8_t *g = glyph(c);
  for (int row = 0; row < 7; ++row)
  {
    for (int col = 0; col < 5; ++col)
    {
      if (g[row] & (1 << (4 - col)))
      {
        fill_rect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

static void draw_text(int x, int y, const char *s, int scale, uint16_t color)
{
  while (*s)
  {
    draw_char(x, y, *s++, scale, color);
    x += 6 * scale;
  }
}

static void draw_text_centered(int y, const char *s, int scale, uint16_t color)
{
  int w = text_width(s, scale);
  draw_text((LCD_W - w) / 2, y, s, scale, color);
}

static const uint8_t seg_map[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static void draw_segment_digit(int x, int y, int digit, uint16_t color, uint16_t off)
{
  const int w = 68, h = 116, t = 11;
  uint8_t m = (digit >= 0 && digit <= 9) ? seg_map[digit] : 0;
  // a,b,c,d,e,f,g
  fill_rect(x+t, y, w-2*t, t, (m&0x01)?color:off);
  fill_rect(x+w-t, y+t, t, h/2-t, (m&0x02)?color:off);
  fill_rect(x+w-t, y+h/2, t, h/2-t, (m&0x04)?color:off);
  fill_rect(x+t, y+h-t, w-2*t, t, (m&0x08)?color:off);
  fill_rect(x, y+h/2, t, h/2-t, (m&0x10)?color:off);
  fill_rect(x, y+t, t, h/2-t, (m&0x20)?color:off);
  fill_rect(x+t, y+h/2-t/2, w-2*t, t, (m&0x40)?color:off);
}

static void draw_large_value(unsigned value, uint16_t color)
{
  if (value > 999) value = 999;

  int hundreds = value / 100;
  int tens = (value / 10) % 10;
  int ones = value % 10;
  int x0 = 47;
  uint16_t off = 0x1082;

  draw_segment_digit(x0, 52, value >= 100 ? hundreds : -1, color, off);
  draw_segment_digit(x0 + 79, 52, value >= 10 ? tens : -1, color, off);
  draw_segment_digit(x0 + 158, 52, ones, color, off);
}

static void draw_speed(float speed, uint16_t color)
{
  float safe_speed = fmaxf(0.0f, speed);
  draw_large_value((unsigned)lroundf(safe_speed), color);
}

static void draw_trip_distance(float distance_m, uint16_t color)
{
  float display_distance = fmaxf(0.0f, distance_m);
  if (display_distance >= 1000.0f)
  {
    display_distance /= 1000.0f;
  }
  draw_large_value((unsigned)lroundf(display_distance), color);
}

static void draw_distance_text_at(int x, int y, float distance_m, int scale, uint16_t color)
{
  unsigned distance = (unsigned)lroundf(fmaxf(0.0f, distance_m));
  char text[24];
  if (distance_m < 1000.0f)
  {
    snprintf(text, sizeof(text), "%u M", distance);
  }
  else if (distance_m < 100000.0f)
  {
    snprintf(text, sizeof(text), "%.2f KM", distance_m / 1000.0f);
  }
  else
  {
    snprintf(text, sizeof(text), "%.1f KM", distance_m / 1000.0f);
  }
  draw_text(x, y, text, scale, color);
}

static void draw_battery(int pct, bool charging)
{
  int x = 256, y = 6, w = 42, h = 16;
  fill_rect(x, y, w, h, LCD_COLOR_WHITE);
  fill_rect(x+2, y+2, w-4, h-4, LCD_COLOR_BLACK);
  fill_rect(x+w, y+5, 3, 6, LCD_COLOR_WHITE);

  int p = pct;
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  uint16_t c = p <= 25 ? LCD_COLOR_RED : (p <= 50 ? LCD_COLOR_YELLOW : LCD_COLOR_GREEN);
  int fill = (w - 6) * p / 100;
  if (fill > 0) fill_rect(x+3, y+3, fill, h-6, c);
  if (charging) draw_text(235, 7, "+", 1, LCD_COLOR_CYAN);
}

static void draw_static_frame(void)
{
  lcd_clear(LCD_COLOR_BLACK);
  fill_rect(0, 29, LCD_W, 1, LCD_COLOR_DARKGREY);
  fill_rect(0, 182, LCD_W, 1, LCD_COLOR_DARKGREY);
}

void lcd_color_test(void)
{
  //-- With Display Inversion now forced off in lcd_init(), these true
  //-- LCD_COLOR_* values should render as their intended colors.
  static const struct { const char *name; uint16_t raw; } bars[] = {
    { "RED",     LCD_COLOR_RED },
    { "GREEN",   LCD_COLOR_GREEN },
    { "BLUE",    LCD_COLOR_BLUE },
    { "YELLOW",  LCD_COLOR_YELLOW },
    { "CYAN",    LCD_COLOR_CYAN },
    { "MAGENTA", LCD_COLOR_MAGENTA },
    { "WHITE",   LCD_COLOR_WHITE },
  };
  const int count = sizeof(bars) / sizeof(bars[0]);
  const int bar_height = LCD_H / count;

  lcd_clear(LCD_COLOR_BLACK);
  for (int i = 0; i < count; ++i)
  {
    int y = i * bar_height;
    fill_rect(0, y, LCD_W, bar_height, bars[i].raw);
    draw_text(4, y + (bar_height - 14) / 2, bars[i].name, 2, LCD_COLOR_BLACK);
  }
}

esp_err_t lcd_init(void)
{
  gpio_config_t io = {
    .pin_bit_mask = (1ULL << LCD_DC) | (1ULL << LCD_RST) | (1ULL << LCD_BL),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&io));
  gpio_set_level(LCD_BL, 0);

  spi_bus_config_t bus = {
    .mosi_io_num = LCD_MOSI,
    .miso_io_num = LCD_MISO,
    .sclk_io_num = LCD_SCLK,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 4096,
  };
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

  spi_device_interface_config_t dev = {
    .clock_speed_hz = 40000000,
    .mode = 0,
    .spics_io_num = LCD_CS,
    .queue_size = 1,
  };
  ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &dev, &s_spi));

  gpio_set_level(LCD_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(LCD_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(120));

  cmd(0x01); vTaskDelay(pdMS_TO_TICKS(120));
  cmd(0x28);
  cmd(0xCF); { const uint8_t d[] = {0x00,0xC1,0x30}; data(d,sizeof(d)); }
  cmd(0xED); { const uint8_t d[] = {0x64,0x03,0x12,0x81}; data(d,sizeof(d)); }
  cmd(0xE8); { const uint8_t d[] = {0x85,0x00,0x78}; data(d,sizeof(d)); }
  cmd(0xCB); { const uint8_t d[] = {0x39,0x2C,0x00,0x34,0x02}; data(d,sizeof(d)); }
  cmd(0xF7); data8(0x20);
  cmd(0xEA); { const uint8_t d[] = {0x00,0x00}; data(d,sizeof(d)); }
  cmd(0xC0); data8(0x23);
  cmd(0xC1); data8(0x10);
  cmd(0xC5); { const uint8_t d[] = {0x3E,0x28}; data(d,sizeof(d)); }
  cmd(0xC7); data8(0x86);
  cmd(0x36); data8(0x08); // buttons below display, normal X direction, BGR
  cmd(0x3A); data8(0x55); // RGB565
  cmd(0xB1); { const uint8_t d[] = {0x00,0x18}; data(d,sizeof(d)); }
  cmd(0xB6); { const uint8_t d[] = {0x08,0x82,0x27}; data(d,sizeof(d)); }
  cmd(0xF2); data8(0x00);
  cmd(0x26); data8(0x01);
  //-- 0x20 (INVOFF) had no visible effect on this panel; try 0x21 (INVON)
  //-- since some ILI9341/9342 clones have inversion semantics reversed.
  cmd(0x21);
  cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));
  cmd(0x29); vTaskDelay(pdMS_TO_TICKS(20));

  draw_static_frame();
  gpio_set_level(LCD_BL, 1);
  s_force_redraw = true;
  s_have_prev = false;
  ESP_LOGI(TAG, "ILI9342C initialized 320x240");
  return ESP_OK;
}

void lcd_set_backlight(bool on)
{
  gpio_set_level(LCD_BL, on ? 1 : 0);
}

void lcd_force_redraw(void)
{
  s_force_redraw = true;
}

void lcd_render(const lcd_view_t *v)
{
  if (!v) return;

  bool full = s_force_redraw || !s_have_prev;
  if (full)
  {
    draw_static_frame();
    s_force_redraw = false;
  }

  if (v->menu_action)
  {
    if (full || !s_prev.menu_action || v->action_selection != s_prev.action_selection ||
        v->storage_available != s_prev.storage_available ||
        v->storage_total_bytes != s_prev.storage_total_bytes ||
        v->storage_free_bytes != s_prev.storage_free_bytes)
    {
      const char *action = "UNKNOWN";
      switch (v->action_selection)
      {
        case 0: action = "RESET TRIP"; break;
        case 1: action = "USED FREE"; break;
        case 2: action = "FORMAT SD"; break;
        case 3: action = "EXIT"; break;
        default: break;
      }
      fill_rect(0, 0, LCD_W, LCD_H, LCD_COLOR_BLACK);
      if (v->action_selection == 1)
      {
        draw_text_centered(72, "SD CARD", 3, LCD_COLOR_CYAN);
        if (v->storage_available)
        {
          uint64_t total_kb = v->storage_total_bytes / 1024;
          uint64_t free_kb = v->storage_free_bytes / 1024;
          char storage[48];
          snprintf(storage, sizeof(storage), "USED:%llu KB", total_kb - free_kb);
          draw_text_centered(120, storage, 2, LCD_COLOR_WHITE);
          snprintf(storage, sizeof(storage), "FREE:%llu KB", free_kb);
          draw_text_centered(154, storage, 2, LCD_COLOR_GREEN);
        }
        else
        {
          draw_text_centered(132, "SD UNAVAILABLE", 2, LCD_COLOR_RED);
        }
      }
      else
      {
        draw_text_centered(92, "EXECUTING", 3, LCD_COLOR_CYAN);
        draw_text_centered(130, action, 3, LCD_COLOR_YELLOW);
      }
    }
    s_prev = *v;
    s_have_prev = true;
    return;
  }

  if (v->system_menu)
  {
    if (full || !s_prev.system_menu ||
        v->storage_total_bytes != s_prev.storage_total_bytes ||
        v->storage_free_bytes != s_prev.storage_free_bytes ||
        v->storage_available != s_prev.storage_available ||
        v->storage_details != s_prev.storage_details ||
        v->menu_selection != s_prev.menu_selection)
    {
      fill_rect(0, 31, LCD_W, 209, LCD_COLOR_BLACK);
      draw_text_centered(42, "SYSTEM MENU", 3, LCD_COLOR_CYAN);
      uint16_t reset_color = v->menu_selection == 0 ? LCD_COLOR_PURPLE : LCD_COLOR_YELLOW;
      uint16_t storage_color = v->menu_selection == 1 ? LCD_COLOR_PURPLE : LCD_COLOR_YELLOW;
      uint16_t format_color = v->menu_selection == 2 ? LCD_COLOR_PURPLE : LCD_COLOR_YELLOW;
      uint16_t exit_color = v->menu_selection == 3 ? LCD_COLOR_PURPLE : LCD_COLOR_YELLOW;
      draw_text(18, 74, "A RESET TRIP", 2, reset_color);
      draw_text(18, 108, "B USED FREE", 2, storage_color);
      draw_text(18, 142, "C FORMAT SD", 2, format_color);
      draw_text(18, 176, "Z EXIT", 2, exit_color);

    }
    s_prev = *v;
    s_have_prev = true;
    return;
  }

  if (full || v->gps_fix != s_prev.gps_fix || v->satellites != s_prev.satellites ||
      v->battery_pct != s_prev.battery_pct || v->charging != s_prev.charging)
  {
    fill_rect(0, 0, LCD_W, 28, LCD_COLOR_BLACK);
    draw_text(7, 7, v->gps_fix ? "GPS" : "NO GPS", 2,
              v->gps_fix ? LCD_COLOR_GREEN : LCD_COLOR_RED);
    char sat[16];
    snprintf(sat, sizeof(sat), "SAT:%02u", v->satellites);
    draw_text(101, 7, sat, 2, LCD_COLOR_WHITE);
    if (v->battery_pct >= 0)
    {
      char bat[12];
      snprintf(bat, sizeof(bat), "%d%%", v->battery_pct);
      draw_text(205, 7, bat, 2, LCD_COLOR_WHITE);
    }
    draw_battery(v->battery_pct, v->charging);
  }

  int current = (int)lroundf(v->speed_kmh);
  int previous = s_have_prev ? (int)lroundf(s_prev.speed_kmh) : -1;
  unsigned dist_current = (unsigned)lroundf(v->distance_m);
  unsigned dist_previous = s_have_prev ? (unsigned)lroundf(s_prev.distance_m) : ~0U;
  bool units_changed = s_have_prev && ((v->distance_m < 1000.0f) != (s_prev.distance_m < 1000.0f));

  if (full || v->trip_mode != s_prev.trip_mode || v->average_mode != s_prev.average_mode ||
      (v->trip_mode && (dist_current != dist_previous || units_changed)))
  {
    fill_rect(0, 31, LCD_W, 150, LCD_COLOR_BLACK);
    if (v->trip_mode)
    {
      draw_text_centered(32, "TRIP", 2, LCD_COLOR_YELLOW);
      draw_trip_distance(v->distance_m, LCD_COLOR_YELLOW);
    }
    else
    {
      draw_text_centered(32, v->average_mode ? "AVG SPEED" : "SPEED", 2, LCD_COLOR_CYAN);
    }
  }

  if (!v->trip_mode && (full || current != previous || v->trip_mode != s_prev.trip_mode ||
                        v->average_mode != s_prev.average_mode))
  {
    draw_speed(v->speed_kmh, v->average_mode ? LCD_COLOR_CYAN : LCD_COLOR_WHITE);
  }

  if (!v->trip_mode && (full || dist_current != dist_previous || v->total_mode != s_prev.total_mode || units_changed ||
                        v->trip_mode != s_prev.trip_mode))
  {
    fill_rect(0, 184, LCD_W, 47, LCD_COLOR_BLACK);
    draw_text(5, 191, "TRIP", 2, LCD_COLOR_YELLOW);
    draw_distance_text_at(86, 190, v->distance_m, 3, LCD_COLOR_YELLOW);
  }
  else if (v->trip_mode && (full || v->trip_mode != s_prev.trip_mode ||
                            current != previous || v->average_mode != s_prev.average_mode))
  {
    fill_rect(0, 184, LCD_W, 47, LCD_COLOR_BLACK);
    const char *speed_label = v->average_mode ? "AVG SPEED" : "SPEED";
    int speed_x = v->average_mode ? 5 + text_width(speed_label, 2) + 10 : 86;
    draw_text(5, 191, speed_label, 2, LCD_COLOR_CYAN);
    char speed[16];
    snprintf(speed, sizeof(speed), "%u KM/H", current < 0 ? 0U : (unsigned)current);
    draw_text(speed_x, 190, speed, 3, LCD_COLOR_WHITE);
  }

  if (full || v->storage_available != s_prev.storage_available ||
      v->storage_free_percent != s_prev.storage_free_percent)
  {
    fill_rect(0, 232, LCD_W, 8, LCD_COLOR_BLACK);
    if (v->storage_available)
    {
      uint16_t bar_width = 220;
      uint16_t free_width = (uint16_t)(bar_width * v->storage_free_percent / 100U);
      uint16_t used_width = bar_width - free_width;
      fill_rect(5, 234, used_width, 4, LCD_COLOR_BLACK);
      fill_rect(5 + used_width, 234, free_width, 4, LCD_COLOR_GREEN);
      char storage[12];
      snprintf(storage, sizeof(storage), "SD %u%%", v->storage_free_percent);
      draw_text(232, 232, storage, 1, LCD_COLOR_WHITE);
    }
    else
    {
      draw_text(5, 232, "SD ERR", 1, LCD_COLOR_RED);
    }
  }

  s_prev = *v;
  s_have_prev = true;
}
