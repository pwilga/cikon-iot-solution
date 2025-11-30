#pragma once
#include <iot_button.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_BUTTONS 8

void button_manager_init(uint8_t gpio_num);
void button_manager_init_multi(const uint8_t *gpio_list, uint8_t num_buttons);

#ifdef __cplusplus
}
#endif
