#include <stdio.h>
#include "led.h"



void led_init(void)
{
    gpio_config_t led_pin_config;
    led_pin_config.pin_bit_mask = 1ULL<<LED_PIN;
    led_pin_config.mode = GPIO_MODE_OUTPUT;
    led_pin_config.pull_up_en = GPIO_PULLUP_DISABLE;
    led_pin_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    led_pin_config.intr_type=GPIO_INTR_DISABLE;
    gpio_config(&led_pin_config);
}

void led_on(void)
{    
    gpio_set_level(LED_PIN, 0);
}

void led_off(void)
{
    gpio_set_level(LED_PIN, 1);
}
