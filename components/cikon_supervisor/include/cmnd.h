#ifndef CMND_H
#define CMND_H

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*command_handler_t)(const char *args_json_str);

typedef struct {
    const char *command_id;  // Command ID (e.g. "restart")
    const char *description; // Command description
    command_handler_t handler;
} command_t;

// Alias for command entry (same structure, used for declaring command groups)
typedef command_t command_entry_t;

typedef struct {
    const command_t *cmnd;
    char *args_json_str;
} command_job_t;

void cmnd_init(QueueHandle_t queue);
void cmnd_process_json(const char *json_string);
void cmnd_register(const char *command_id, const char *description, command_handler_t handler);
void cmnd_unregister(const char *command_id);
void cmnd_register_group(const command_entry_t *commands);
void cmnd_unregister_group(const command_entry_t *commands);
void cmnd_submit(const char *command_id, const char *args_json_str);

const command_t *cmnd_find(const char *command_id);
const command_t *cmnd_get_registry(size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif // CMND_H