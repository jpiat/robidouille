#include <wiringPi.h>

#define LED_PIN 7

void flash_init();
void flash_toggle();
void flash_set_period(unsigned char period);
