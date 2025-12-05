#pragma once

#include "supervisor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RF433 adapter instance
 *
 * Provides 433MHz RF receiver functionality
 * with Home Assistant integration.
 */
extern supervisor_platform_adapter_t rf433_adapter;

#ifdef __cplusplus
}
#endif
