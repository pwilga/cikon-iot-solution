#include "json_parser.h"
#include <string.h>
#include <ctype.h>

bool json_str_as_string_buf(const char *json_str, char *out_buf, size_t buf_size) {
    if (!out_buf || buf_size == 0) {
        return false;
    }

    cJSON *json_root = cJSON_Parse(json_str);
    if (!json_root) {
        return false;
    }

    bool success = false;
    if (cJSON_IsString(json_root) && json_root->valuestring) {
        strncpy(out_buf, json_root->valuestring, buf_size - 1);
        out_buf[buf_size - 1] = '\0';
        success = true;
    }

    cJSON_Delete(json_root);
    return success;
}

int json_str_as_int(const char *json_str) {
    cJSON *json_root = cJSON_Parse(json_str);
    if (!json_root)
        return 0;

    int value = 0;
    if (cJSON_IsNumber(json_root)) {
        value = json_root->valueint;
    }

    cJSON_Delete(json_root);
    return value;
}

bool json_str_as_bool(const char *json_str) {
    cJSON *json_root = cJSON_Parse(json_str);
    if (!json_root)
        return false;

    bool result = cJSON_IsTrue(json_root);
    cJSON_Delete(json_root);
    return result;
}

cJSON *json_str_as_object(const char *json_str) {
    cJSON *json_root = cJSON_Parse(json_str);
    if (!json_root || !cJSON_IsObject(json_root)) {
        cJSON_Delete(json_root);
        return NULL;
    }

    return json_root;
}

logic_state_t json_str_as_logic_state(const char *json_str) {

    cJSON *json_root = cJSON_Parse(json_str);
    if (!json_root) {
        return STATE_OFF;
    }

    logic_state_t result = STATE_OFF;

    if (cJSON_IsBool(json_root)) {
        result = cJSON_IsTrue(json_root) ? STATE_ON : STATE_OFF;

    } else if (cJSON_IsNumber(json_root)) {
        result = json_root->valuedouble ? STATE_ON : STATE_OFF;

    } else if (cJSON_IsString(json_root)) {
        const char *s = json_root->valuestring;
        if (strcasecmp(s, "on") == 0 || strcasecmp(s, "1") == 0 || strcasecmp(s, "true") == 0) {
            result = STATE_ON;
        } else if (strcasecmp(s, "toggle") == 0) {
            result = STATE_TOGGLE;
        }
    }

    cJSON_Delete(json_root);
    return result;
}

char *sanitize(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out)
        return NULL;

    for (size_t i = 0; i < len; i++) {
        if (s[i] == ' ')
            out[i] = '_';
        else if (isupper((unsigned char)s[i]))
            out[i] = tolower((unsigned char)s[i]);
        else
            out[i] = s[i];
    }
    out[len] = '\0';
    return out;
}
