#pragma once

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "stdbool.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mqtt_command_callback_t)(const char *payload);
// Returns the telemetry as a JSON string on the heap, which is freed after it is published. NULL
// when it could not be built
typedef char *(*mqtt_telemetry_callback_t)(void);

typedef struct {
    const char *client_id;
    const char *device_name;
    const char *device_manufacturer;
    const char *device_model;
    const char *device_sw_version;
    const char *device_hw_version;
    const char *device_uri;
    const char *mqtt_node;
    const char *mqtt_broker;
    const char *mqtt_user;
    const char *mqtt_pass;
    const char *mqtt_disc_pref;
    uint8_t mqtt_mtls_en;
    uint8_t mqtt_max_retry;
    mqtt_command_callback_t command_cb;
    mqtt_telemetry_callback_t telemetry_cb;
} mqtt_config_t;

void mqtt_configure(const mqtt_config_t *cfg);
void mqtt_init(void);
void mqtt_shutdown(void);

void mqtt_publish(const char *topic, const char *payload, int qos, bool retain);
void mqtt_publish_offline_state(void);
bool mqtt_is_connected(void);
void mqtt_trigger_telemetry(void);
const mqtt_config_t *mqtt_get_config(void);

void mqtt_log_event_group_bits(void);

#ifdef __cplusplus
}
#endif
