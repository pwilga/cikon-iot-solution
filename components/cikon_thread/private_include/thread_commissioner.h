#pragma once

#include "sdkconfig.h"

#include "cJSON.h"

void thread_commissioner_handler(const char *args_json_str);

/**
 * Adds "commissioner": {"state": "disabled"|"petition"|"active", "timeout": seconds the window was
 * opened for and "remaining": seconds left, both only when active, "error": why the last start
 * failed, only after a failure} to the "thread" tele object. Does nothing when OpenThread has no
 * commissioner.
 */
#if CONFIG_OPENTHREAD_COMMISSIONER
void thread_commissioner_tele(cJSON *thread);
#else
static inline void thread_commissioner_tele(cJSON *thread) { (void)thread; }
#endif
