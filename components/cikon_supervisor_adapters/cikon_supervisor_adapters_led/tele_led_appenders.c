#include "cJSON.h"

#include "json_parser.h"
#include "led_adapter.h"
#include "tele.h"
#include "tele_led_appenders.h"

static void led_tele_appender(const char *tele_id, cJSON *json_root) {
    cJSON *led_obj = cJSON_CreateObject();

    for (uint8_t i = 0;; i++) {
        const char *name = led_get_name(i);
        if (!name) {
            break;
        }
        uint8_t brightness = led_get_brightness(i);
        char *sanitized_name = sanitize(name);
        cJSON_AddNumberToObject(led_obj, sanitized_name, brightness);
        free(sanitized_name);
    }

    cJSON_AddItemToObject(json_root, tele_id, led_obj);
}

void led_tele_appenders_register(void) { tele_register("pwm_led", led_tele_appender); }
