#pragma once

#include "driver/gpio.h"

// Classic ESP32 Dev Kit: DS18B20 DATA needs a bidirectional digital GPIO.
// GPIO16/27 avoid flash, boot straps, and UART0. Pull DATA up to 3.3 V.
#define BOARD_TEMPERATURE_SENSOR_GPIO GPIO_NUM_27
#define BOARD_RELAY_GPIO GPIO_NUM_16
