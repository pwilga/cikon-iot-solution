#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "cJSON.h"

#include "config_fields_private.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file config_manager.h
 * @brief Configuration manager for persistent device settings using X-macro and NVS (ESP-IDF).
 *
 * All configuration fields are defined in one place (CONFIG_FIELDS X-macro).
 * This enables automatic generation of the config struct, setters, and NVS integration.
 *
 * IMPORTANT: Each field/key name MUST be max 15 characters (NVS key limit in ESP-IDF).
 */

/**
 * @brief Structure holding all persistent configuration fields.
 *
 * Fields are generated from CONFIG_FIELDS X-macro.
 * Do not modify directly; use setters to update and save fields.
 */
typedef struct {
#define STR(field, size, defval) char field[size];
#define U8(field, defval) uint8_t field;
#define U16(field, defval) uint16_t field;
    CONFIG_FIELDS(STR, U8, U16)
#undef STR
#undef U8
#undef U16
} config_t;

/**
 * @name Field Setters
 * @brief Set a configuration field and save it to NVS.
 *
 * Each setter updates only one field and commits the change to NVS.
 * Use these instead of modifying the struct directly.
 */
///@{
/** Set string field and save to NVS. */
#define GEN_SETTER_STR(field, size, defval) esp_err_t config_set_##field(const char *val);
/** Set uint8_t field and save to NVS. */
#define GEN_SETTER_U8(field, defval) esp_err_t config_set_##field(uint8_t val);
/** Set uint16_t field and save to NVS. */
#define GEN_SETTER_U16(field, defval) esp_err_t config_set_##field(uint16_t val);
CONFIG_FIELDS(GEN_SETTER_STR, GEN_SETTER_U8, GEN_SETTER_U16)
#undef GEN_SETTER_STR
#undef GEN_SETTER_U8
#undef GEN_SETTER_U16
///@}

/**
 * @brief Initialize configuration: load from NVS or set defaults if not present.
 *
 * Call once at startup before using any config accessors or setters.
 */
void config_manager_init(void);

/**
 * @brief Get a pointer to the current configuration (read-only).
 *
 * @return Pointer to config_t structure with current values.
 */
const config_t *config_get(void);

/**
 * @brief Log all NVS keys in the configuration namespace (for diagnostics).
 */
void config_manager_print_all_keys(void);

/**
 * @brief Set configuration fields from a cJSON object.
 *
 * For each key in the JSON, if it matches a config field, the corresponding setter is called.
 * If the key is unknown, a warning is logged.
 */
void config_manager_set_from_json(const cJSON *json);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_MANAGER_H
