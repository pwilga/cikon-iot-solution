#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "openthread/dataset.h"

#include "cmnd.h"

bool thread_dataset_parse_hex(const char *hex, otOperationalDatasetTlvs *out);

/**
 * @brief Supervisor commands of the Thread services built into this device.
 *
 * Each service is there only when its OpenThread option is on; without any the array holds just
 * the terminator.
 */
extern const command_entry_t thread_commands[];
