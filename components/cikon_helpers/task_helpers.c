#include "task_helpers.h"

bool task_wait_for_finish(volatile TaskHandle_t *task_handle, uint32_t timeout_ms) {
    if (task_handle == NULL || *task_handle == NULL) {
        return true; // Already finished
    }

    uint32_t wait_count = 0;
    uint32_t max_count = timeout_ms / 10;

    while (*task_handle != NULL && wait_count < max_count) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_count++;
    }

    return (*task_handle == NULL);
}
