#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum
{
  BOARD_BUTTON_A = 0,
  BOARD_BUTTON_B,
  BOARD_BUTTON_C,
} board_button_t;

typedef struct
{
  board_button_t button;
  bool long_press;
} board_button_event_t;

esp_err_t board_init(void);
bool board_get_button_event(board_button_event_t *event);
int board_battery_level(void);
bool board_is_charging(void);
