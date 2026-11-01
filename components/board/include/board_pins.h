#pragma once

#include "driver/gpio.h"

// Classic ESP32 Dev Kit: DS18B20 DATA needs a bidirectional digital GPIO.
// GPIO25/26/27 avoid flash, boot straps, and UART0. Pull DATA up to 3.3 V.
#define BOARD_TEMPERATURE_SENSOR_GPIO GPIO_NUM_27
#define BOARD_HEATER_GPIO GPIO_NUM_25
#define BOARD_FAN_GPIO GPIO_NUM_26
