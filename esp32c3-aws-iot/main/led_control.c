#include "led_control.h"

#include "esp_log.h"

static const char *TAG = "led_control";
static bool s_led_state = false;

esp_err_t led_control_init(void)
{
    gpio_reset_pin(LED_GPIO_PIN);
    gpio_set_direction(LED_GPIO_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO_PIN, 0);
    s_led_state = false;
    ESP_LOGI(TAG, "GPIO%d configured as output", LED_GPIO_PIN);
    return ESP_OK;
}

void led_control_set(bool on)
{
    gpio_set_level(LED_GPIO_PIN, on ? 1 : 0);
    s_led_state = on;
    ESP_LOGI(TAG, "LED %s", on ? "ON" : "OFF");
}

bool led_control_get(void)
{
    return s_led_state;
}
