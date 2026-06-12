#pragma once

#include <stdbool.h>

#include "openthread/dataset.h"

bool thread_dataset_parse_hex(const char *hex, otOperationalDatasetTlvs *out);
void thread_log_network_info(void);

#if CONFIG_OPENTHREAD_CLI
void thread_console_start(const char *prompt);
void thread_cli_commands_init(void);
#endif
