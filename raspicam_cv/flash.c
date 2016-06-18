#include "flash.h"

unsigned char period_counter = 0 ;
unsigned char period_reload = 0 ;
unsigned char duty_cycle = 0 ;

void flash_init(){
  wiringPiSetup() ;
  pinMode (LED_PIN, OUTPUT) ;
  period_counter = 0 ;
  period_reload =  0;
}
void flash_set_period(unsigned char period){
	period_reload = period ;
}

void flash_set_duty(unsigned char duty){
        duty_cycle = duty ;
}

void flash_update(){
	if(duty_cycle == 0){
		digitalWrite (LED_PIN, LOW);
	}else if(duty_cycle == 255){
		digitalWrite (LED_PIN, HIGH);
	}else if(period_counter > duty_cycle){
		digitalWrite (LED_PIN, HIGH);
	}else  digitalWrite (LED_PIN, LOW);

	if(period_counter == 0) period_counter = period_reload ;
	else period_counter -- ;

}
