#pragma once

#include "supervisor.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern supervisor_platform_adapter_t led_adapter;

void led_set_brightness(uint8_t led_index, uint8_t brightness);
void led_fade_to(uint8_t led_index, uint8_t target, uint32_t duration_ms);
uint8_t led_get_brightness(uint8_t led_index);

void led_turn_on(uint8_t led_index);
void led_turn_off(uint8_t led_index);
bool led_is_on(uint8_t led_index);

int8_t led_find_by_name(const char *name);
const char *led_get_name(uint8_t led_index);

void led_test_sequence(uint8_t led_index);

#ifdef __cplusplus
}
#endif
