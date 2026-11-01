#pragma once

typedef int gpio_num_t;
#define GPIO_NUM_27 27
#define GPIO_NUM_34 34
#define GPIO_IS_VALID_OUTPUT_GPIO(gpio) ((gpio) >= 0 && (gpio) < 34)
int gpio_get_level(gpio_num_t gpio);
