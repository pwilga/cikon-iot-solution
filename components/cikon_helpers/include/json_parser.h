#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { STATE_OFF = 0, STATE_ON, STATE_TOGGLE } logic_state_t;

bool json_str_as_string_buf(const char *json_str, char *out_buf, size_t buf_size);
int json_str_as_int(const char *json_str);
bool json_str_as_bool(const char *json_str);
cJSON *json_str_as_object(const char *json_str);

/**
 * @brief Parses a JSON-encoded argument and converts it to a logic state.
 *
 * Accepts boolean, numeric or string values:
 * - Returns STATE_ON for: true, "true", 1, "1", "on" (case-insensitive), any non-zero number
 * - Returns STATE_TOGGLE for: "toggle" (case-insensitive)
 * - Returns STATE_OFF for any other input or invalid JSON
 *
 * @param json_str JSON string representing a logic state (e.g., "on", 1, true, "toggle")
 * @return logic_state_t Parsed state: STATE_ON, STATE_OFF or STATE_TOGGLE
 */
logic_state_t json_str_as_logic_state(const char *json_str);

/**
 * @brief Returns a sanitized lowercase copy of the input string.
 *
 * Spaces are replaced with underscores, uppercase letters are lowercased.
 *
 * @param s Input null-terminated C string.
 * @return char* Newly allocated sanitized string. Must be freed by caller.
 */
char *sanitize(const char *s);

#ifdef __cplusplus
}
#endif

#endif // JSON_PARSER_H
