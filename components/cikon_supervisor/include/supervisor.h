#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "cmnd.h"
#include "tele.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Supervisor interval stages
 */
typedef enum {
    SUPERVISOR_INTERVAL_1S,
    SUPERVISOR_INTERVAL_2S,
    SUPERVISOR_INTERVAL_5S,
    SUPERVISOR_INTERVAL_10S,
    SUPERVISOR_INTERVAL_30S,
    SUPERVISOR_INTERVAL_60S,
    SUPERVISOR_INTERVAL_5M,
    SUPERVISOR_INTERVAL_10M,
    SUPERVISOR_INTERVAL_2H,
    SUPERVISOR_INTERVAL_12H,
    SUPERVISOR_INTERVAL_COUNT
} supervisor_interval_stage_t;

/**
 * @brief Platform adapter interface for supervisor
 *
 * Allows supervisor to work with different platforms (inet, zigbee, thread, matter)
 * without knowing platform-specific details. Each platform implements this interface.
 */
typedef struct {
    /**
     * @brief Enable this adapter in safe mode
     * If false (default), adapter is skipped when safe mode is active.
     * Only critical adapters (inet) should set this to true.
     */
    bool enable_in_safe_mode;

    /**
     * @brief Initialize platform-specific resources
     * Called once during supervisor startup.
     */
    void (*init)(void);

    /**
     * @brief Shutdown platform gracefully
     * Called before restart or power down.
     */
    void (*shutdown)(void);

    /**
     * @brief Handle platform events
     * @param bits Event bits set by platform (TIME_SYNCED, NETWORK_READY, etc.)
     */
    void (*on_event)(EventBits_t bits);

    /**
     * @brief Handle cyclic intervals
     * @param stage Interval stage (1s, 5s, 60s, 5m, 10m, 2h, 12h)
     */
    void (*on_interval)(supervisor_interval_stage_t stage);

    /**
     * @brief Telemetry appenders group (NULL-terminated)
     * Array of tele_entry_t defining telemetry sources for this adapter.
     * Supervisor automatically registers these during adapter registration.
     */
    const tele_entry_t *tele_group;

    /**
     * @brief Command handlers group (NULL-terminated)
     * Array of command_entry_t defining commands for this adapter.
     * Supervisor automatically registers these during adapter registration.
     */
    const command_entry_t *cmnd_group;

    /**
     * @brief Optional metadata (NULL if unused)
     * Platform-specific metadata (e.g., HA entities, Zigbee endpoints).
     * Format determined by metadata magic signature.
     */
    const void *metadata;

} supervisor_platform_adapter_t;

/**
 * @brief Initialize supervisor core (queue, event group, cmnd, tele)
 * Must be called before registering adapters.
 */
void supervisor_init(void);

/**
 * @brief Register platform adapter
 * @param adapter Pointer to adapter instance (must remain valid)
 * @return ESP_OK on success, ESP_ERR_NO_MEM if too many adapters
 */
esp_err_t supervisor_register_adapter(supervisor_platform_adapter_t *adapter);

/**
 * @brief Initialize all registered platform adapters and start supervisor task
 * @return ESP_OK on success
 */
esp_err_t supervisor_platform_init(void);

/**
 * @brief Get supervisor command queue handle
 * @return Queue handle for submitting commands
 */
QueueHandle_t supervisor_get_queue(void);

/**
 * @brief Get supervisor event group handle
 * @return Event group handle for platform events
 */
EventGroupHandle_t supervisor_get_event_group(void);

/**
 * @brief Notify supervisor of platform event
 * Platform adapters use this to signal events to supervisor.
 * @param bits Event bits to set
 */
void supervisor_notify_event(EventBits_t bits);

/**
 * @brief Get array of registered adapters (NULL-terminated)
 * @return Pointer to NULL-terminated array of adapter pointers
 */
const supervisor_platform_adapter_t **supervisor_get_adapters(void);

/**
 * @brief Check if safe mode is currently active
 * @return true if safe mode is active, false otherwise
 */
bool supervisor_is_safe_mode_active(void);

#ifdef __cplusplus
}
#endif
