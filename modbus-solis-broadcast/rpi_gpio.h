#ifndef RPI_GPIO

#ifdef RPI
#include <wiringPi.h>

// define GPIOs for RS485 control
#define RS485_RE 4
// only define one if RE & DE are tied together
//#define RS485_DE 24

#endif
