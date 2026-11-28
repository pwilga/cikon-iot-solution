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

/**
 * Stops the attempt in progress, for a device that is getting a network some other way; the retries
 * end once it has a dataset. Call with the OpenThread lock held. Does nothing when OpenThread has
 * no joiner.
 */
#if CONFIG_OPENTHREAD_JOINER
void thread_joiner_stop(void);
#else
static inline void thread_joiner_stop(void) {}
#endif
