#pragma once

#include "sdkconfig.h"

/**
 * Starts Thread with the dataset kept in NVS. A device without one only waits for a commissioner
 * that has this PSKd (the joiner tries every 30 s): a network is never created by itself, that is
 * the operator's decision (thread_create). Call after the OpenThread stack has started. It starts
 * in a task of its own and returns at once, @p pskd has to stay valid until then.
 */
void thread_network_start(const char *pskd);

/**
 * Leaves the Thread network and erases its dataset, then restarts the device: the SRP entry is
 * removed from the server and the neighbors are told. Everything else stays in NVS. Takes a few
 * seconds. Does nothing on a device without a network. Supervisor command thread_forget.
 */
void thread_network_forget_handler(const char *args_json_str);

#if CONFIG_OPENTHREAD_FTD
/**
 * Creates a Thread network and starts it: {"name": "...", "channel": 11-26}, both needed. Does
 * nothing on a device that has a network: thread_forget erases it first. Supervisor command
 * thread_create.
 */
void thread_network_create_handler(const char *args_json_str);
#endif
