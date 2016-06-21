#include "flash.h"


unsigned char flash_pins [8] ;
unsigned char * flash_pattern ;
unsigned char flash_pattern_length = 0;
unsigned int nb_output = 0;
unsigned int period_counter ;

unsigned char flash_init(unsigned char pin){
  if(nb_output == 0) wiringPiSetup() ;
  flash_pins[nb_output]= pin ;
  pinMode (flash_pins[nb_output], OUTPUT) ;
  nb_output += 1 ;
  period_counter = 0 ;
  return (nb_output - 1);
}

void flash_set_pattern(unsigned char * pattern, unsigned char pattern_length){
	flash_pattern = pattern ;
	flash_pattern_length = pattern_length ;
}

void flash_update(){
	int i;
	for(i = 0 ; i < nb_output ; i ++){
		unsigned char on_off = (flash_pattern[period_counter]  >> i) & 0x01 ;
		digitalWrite (flash_pins[i], on_off);
	}
	period_counter ++ ;
	if(period_counter >=  flash_pattern_length) period_counter = 0 ;
}
