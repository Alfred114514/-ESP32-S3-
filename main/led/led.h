#ifndef __LED_H
#define __LED_H
 
#include "driver/gpio.h"
#define LED_PIN 48

void led_init(void);
void led_on(void);
void led_off(void);

#endif
