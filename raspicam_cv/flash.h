#include <wiringPi.h>

#define LED_PIN 7

void flash_init();
void flash_update();
void flash_set_period(unsigned char period);
void flash_set_duty(unsigned char duty);
