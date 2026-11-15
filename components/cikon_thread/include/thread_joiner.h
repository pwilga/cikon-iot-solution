#pragma once

#include "sdkconfig.h"

/**
 * Joins a Thread network through a commissioner that has this PSKd. Retries every 30 s until it
 * succeeds, then starts Thread. Call with the OpenThread lock held, for a device without a
 * dataset. Does nothing when OpenThread has no joiner.
 */
#if CONFIG_OPENTHREAD_JOINER
void thread_joiner_start(const char *pskd);
#else
static inline void thread_joiner_start(const char *pskd) { (void)pskd; }
#endif
