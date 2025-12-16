#pragma once

#include "supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief DS18B20 temperature sensor adapter instance
 *
 * Provides 1-Wire DS18B20 temperature sensor support
 * with Home Assistant integration.
 */
extern supervisor_platform_adapter_t ds18b20_adapter;

#ifdef __cplusplus
}
#endif
