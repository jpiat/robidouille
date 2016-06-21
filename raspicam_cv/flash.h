#include <wiringPi.h>



unsigned char flash_init(unsigned char pin);
void flash_update();
void flash_set_pattern(unsigned char * pattern, unsigned char pattern_length);
