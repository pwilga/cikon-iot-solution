#pragma once

#include "supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Button adapter for supervisor
 *
 * Provides physical button input handling with configurable actions
 */
extern supervisor_platform_adapter_t button_adapter;

#ifdef __cplusplus
}
#endif
