#include <wiringPi.h>


void flash_init(unsigned char pin);
void flash_update();
void flash_set_period(unsigned char period);
void flash_set_duty(unsigned char duty);
