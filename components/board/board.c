#include "board.h"

#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define BUTTON_A_GPIO GPIO_NUM_39
#define BUTTON_B_GPIO GPIO_NUM_38
#define BUTTON_C_GPIO GPIO_NUM_37
#define I2C_PORT I2C_NUM_0
#define I2C_SDA GPIO_NUM_21
#define I2C_SCL GPIO_NUM_22
#define IP5306_ADDR 0x75
#define IP5306_REG_READ0 0x70
#define IP5306_REG_READ4 0x78

static const char *TAG = "board";
static QueueHandle_t s_button_queue;

typedef struct
{
  gpio_num_t gpio;
  board_button_t button;
  bool down;
  bool long_press_sent;
  int64_t down_us;
} button_state_t;

static button_state_t s_buttons[] = {
  { BUTTON_A_GPIO, BOARD_BUTTON_A, false, false, 0 },
  { BUTTON_B_GPIO, BOARD_BUTTON_B, false, false, 0 },
  { BUTTON_C_GPIO, BOARD_BUTTON_C, false, false, 0 },
};

static esp_err_t ip5306_read(uint8_t reg, uint8_t *value)
{
  return i2c_master_write_read_device(I2C_PORT, IP5306_ADDR,
                                      &reg, 1, value, 1,
                                      pdMS_TO_TICKS(50));
}

static const char *button_name(board_button_t button)
{
  switch (button)
  {
    case BOARD_BUTTON_A: return "A (LEFT)";
    case BOARD_BUTTON_B: return "B (MIDDLE)";
    case BOARD_BUTTON_C: return "C (RIGHT)";
    default: return "UNKNOWN";
  }
}

static void button_task(void *arg)
{
  (void)arg;
  const int64_t debounce_us = 30000;
  const int64_t long_press_us = 800000;

  while (true)
  {
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i)
    {
      button_state_t *b = &s_buttons[i];
      bool pressed = gpio_get_level(b->gpio) == 0;
      int64_t now = esp_timer_get_time();

      if (pressed && !b->down)
      {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (gpio_get_level(b->gpio) == 0)
        {
          b->down = true;
          b->down_us = now;
          b->long_press_sent = false;
        }
      }
      else if (pressed && b->down)
      {
        int64_t held = now - b->down_us;
        if (!b->long_press_sent && held >= long_press_us)
        {
          board_button_event_t event = {
            .button = b->button,
            .long_press = true,
          };
          ESP_LOGI(TAG, "Button %s: LONG press threshold reached (%lld ms)",
                   button_name(event.button),
                   held / 1000);
          xQueueSend(s_button_queue, &event, 0);
          b->long_press_sent = true;
        }
      }
      else if (!pressed && b->down)
      {
        int64_t held = now - b->down_us;
        if (held >= debounce_us && !b->long_press_sent)
        {
          board_button_event_t event = {
            .button = b->button,
            .long_press = false,
          };
          ESP_LOGI(TAG, "Button %s: SHORT press (%lld ms)",
                   button_name(event.button),
                   held / 1000);
          xQueueSend(s_button_queue, &event, 0);
        }
        b->down = false;
        b->long_press_sent = false;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

esp_err_t board_init(void)
{
  gpio_config_t buttons = {
    .pin_bit_mask = (1ULL << BUTTON_A_GPIO) | (1ULL << BUTTON_B_GPIO) | (1ULL << BUTTON_C_GPIO),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&buttons));

  i2c_config_t i2c = {
    .mode = I2C_MODE_MASTER,
    .sda_io_num = I2C_SDA,
    .scl_io_num = I2C_SCL,
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
    .master.clk_speed = 400000,
    .clk_flags = 0,
  };
  ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &i2c));
  ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

  s_button_queue = xQueueCreate(8, sizeof(board_button_event_t));
  if (!s_button_queue)
  {
    return ESP_ERR_NO_MEM;
  }

  xTaskCreate(button_task, "buttons", 2048, NULL, 8, NULL);
  ESP_LOGI(TAG, "Buttons and IP5306 initialized");
  return ESP_OK;
}

bool board_get_button_event(board_button_event_t *event)
{
  if (!event || !s_button_queue)
  {
    return false;
  }
  return xQueueReceive(s_button_queue, event, 0) == pdTRUE;
}

int board_battery_level(void)
{
  uint8_t value = 0;
  if (ip5306_read(IP5306_REG_READ4, &value) != ESP_OK)
  {
    return -1;
  }

  switch (value & 0xF0)
  {
    case 0x00: return 100;
    case 0x80: return 75;
    case 0xC0: return 50;
    case 0xE0: return 25;
    default: return 0;
  }
}

bool board_is_charging(void)
{
  uint8_t value = 0;
  if (ip5306_read(IP5306_REG_READ0, &value) != ESP_OK)
  {
    return false;
  }
  return (value & 0x08) != 0;
}
