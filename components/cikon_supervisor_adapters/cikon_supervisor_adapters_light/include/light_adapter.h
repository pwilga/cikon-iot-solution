#pragma once

#include "supervisor.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern supervisor_platform_adapter_t light_adapter;

void light_set_state(const char *name, bool on);
bool light_get_state(const char *name);

#ifdef __cplusplus
}
#endif
