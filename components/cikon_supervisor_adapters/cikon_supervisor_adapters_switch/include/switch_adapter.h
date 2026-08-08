#pragma once

#include "supervisor.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern supervisor_platform_adapter_t switch_adapter;

void switch_set_state(const char *name, bool on);
bool switch_get_state(const char *name);
void switch_toggle(const char *name);

#ifdef __cplusplus
}
#endif
