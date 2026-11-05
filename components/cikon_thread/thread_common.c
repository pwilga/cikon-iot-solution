#include "thread_common.h"
#include "openthread/dataset.h"
#include "thread_commissioner.h" // IWYU pragma: keep
#include <string.h>

bool thread_dataset_parse_hex(const char *hex, otOperationalDatasetTlvs *out) {
    size_t hex_len = strlen(hex);
    if (hex_len == 0 || hex_len % 2 != 0 || hex_len / 2 > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < hex_len; i += 2) {
        char byte_str[3] = {hex[i], hex[i + 1], '\0'};
        char *end;
        long val = strtol(byte_str, &end, 16);
        if (*end != '\0') {
            return false;
        }
        out->mTlvs[i / 2] = (uint8_t)val;
    }
    out->mLength = (uint8_t)(hex_len / 2);
    return true;
}

const command_entry_t thread_commands[] = {
#if CONFIG_OPENTHREAD_COMMISSIONER
    {"commissioner", "Let joiners that know the PSKd join the Thread network (on/off)",
     thread_commissioner_handler},
#endif
    {NULL, NULL, NULL}};
