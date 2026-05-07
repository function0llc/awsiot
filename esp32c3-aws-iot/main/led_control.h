#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

#define LED_GPIO_PIN GPIO_NUM_5

esp_err_t led_control_init(void);
void led_control_set(bool on);
bool led_control_get(void);
