#pragma once

#include "supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LED Indicator adapter for supervisor
 *
 * Provides LED status indication using espressif/led_indicator component
 * with blink patterns, priorities, and preemptive control
 */
extern supervisor_platform_adapter_t led_indicator_adapter;

#ifdef __cplusplus
}
#endif
