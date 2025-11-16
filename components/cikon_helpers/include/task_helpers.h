#ifndef TASK_HELPERS_H
#define TASK_HELPERS_H

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Safely wait for a FreeRTOS task to finish with timeout
 *
 * @param task_handle Pointer to TaskHandle_t (will be set to NULL by the task on exit)
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return true if task finished within timeout, false if timeout occurred
 */
bool task_wait_for_finish(volatile TaskHandle_t *task_handle, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // TASK_HELPERS_H
