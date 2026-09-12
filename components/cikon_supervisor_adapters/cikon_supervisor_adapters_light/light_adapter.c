#include <stdio.h>

#include "esp_log.h"

#include "cJSON.h"

#include "cmnd.h"
#include "json_parser.h"
#include "light.h"
#include "light_adapter.h"
#include "light_generated.h" // LIGHT_CMND_LIST, HA_ENTITY_LIST - one entry per configured light
#include "metadata.h"
#include "supervisor.h"
#include "tele.h"

#define TAG "cikon:adapter:light"

// Kelvin range Home Assistant is told this light covers, mapped onto the driver's cct
// cool-share 0-100. The driver has no notion of kelvin.
#define LIGHT_KELVIN_MIN 2200
#define LIGHT_KELVIN_MAX 7000

// One cmnd is registered per configured light, each pointing at its own trampoline below -
// command_handler_t carries no context, so a single shared handler can't tell which light it
// was called for. LIGHT_CMND_LIST comes from light_generated.h, built from the same lights.toml
// as the driver's own table, so the Nth trampoline is the Nth light by construction.

// Decodes one cmnd payload into a change and hands it over. Two payload shapes are accepted:
// an object naming any subset of the fields, or a bare on/off/toggle string.
static void cmnd_light_apply(size_t index, const char *args_json_str) {
    cJSON *root = cJSON_Parse(args_json_str);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON: %s", args_json_str);
        return;
    }

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        logic_state_t state = json_str_as_logic_state(args_json_str);
        if (state == STATE_TOGGLE) {
            light_toggle(index);
        } else {
            light_set_on(index, state == STATE_ON);
        }
        return;
    }

    light_state_change_t change = {0};
    cJSON *item;

    if ((item = cJSON_GetObjectItem(root, "h"))) {
        change.fields |= LIGHT_FIELD_HUE;
        change.hue = (uint16_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "s"))) {
        change.fields |= LIGHT_FIELD_SATURATION;
        change.saturation = (uint8_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "v"))) {
        change.fields |= LIGHT_FIELD_BRIGHTNESS;
        change.brightness = (uint8_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "cct"))) {
        change.fields |= LIGHT_FIELD_CCT;
        change.cct = (uint16_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "on"))) {
        change.fields |= LIGHT_FIELD_ON;
        change.on = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(root, "effect")) && cJSON_IsString(item)) {
        change.fields |= LIGHT_FIELD_EFFECT;
        change.effect = item->valuestring;
    }
    if ((item = cJSON_GetObjectItem(root, "speed"))) {
        change.fields |= LIGHT_FIELD_EFFECT_SPEED;
        change.effect_speed = (uint8_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "intensity"))) {
        change.fields |= LIGHT_FIELD_EFFECT_INTENSITY;
        change.effect_intensity = (uint8_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "h2"))) {
        change.fields |= LIGHT_FIELD_HUE2;
        change.hue2 = (uint16_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "s2"))) {
        change.fields |= LIGHT_FIELD_SATURATION2;
        change.saturation2 = (uint8_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "v2"))) {
        change.fields |= LIGHT_FIELD_BRIGHTNESS2;
        change.brightness2 = (uint8_t)item->valueint;
    }

    // Before the delete: change.effect points into root.
    light_apply_change(index, &change);
    cJSON_Delete(root);
}

#define X(n)                                                                                       \
    static void cmnd_light_##n(const char *args_json_str) {                                        \
        cmnd_light_apply(n, args_json_str);                                                        \
    }
LIGHT_CMND_LIST
#undef X

#define X(n) cmnd_light_##n,
static const command_handler_t cmnd_light_trampolines[] = {LIGHT_CMND_LIST};
#undef X

static void light_adapter_on_interval(supervisor_interval_stage_t stage) {
    if (stage == SUPERVISOR_INTERVAL_10S) {
        light_save_state();
    }
}


// The wire format each light accepts, from what it can do.
static const char *light_adapter_cmnd_description(const light_caps_t *caps) {
    if (caps->is_switch) {
        return "Set switch state (on/off/toggle)";
    }
    if (caps->has_color && caps->has_cct) {
        return "Set light color/CCT/brightness/state ({h,s,v,cct,on} or on/off/toggle)";
    }
    if (caps->has_color) {
        return "Set light color/brightness/state ({h,s,v,on} or on/off/toggle)";
    }
    if (caps->has_cct) {
        return "Set light CCT/brightness/state ({cct,v,on} or on/off/toggle)";
    }
    return "Set light brightness/state ({v,on} or on/off/toggle)";
}

static esp_err_t light_adapter_init(void) {

    ESP_LOGI(TAG, "Initializing light adapter");

    esp_err_t err = light_init();
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < light_count(); i++) {
        light_caps_t caps;
        light_get_caps(i, &caps);
        cmnd_register(light_name(i), light_adapter_cmnd_description(&caps),
                      cmnd_light_trampolines[i]);
    }

    ESP_LOGI(TAG, "Light adapter initialized");
    return ESP_OK;
}



static esp_err_t light_adapter_shutdown(void) {
    // Every command goes before any teardown, so none can arrive while a strip handle is
    // being freed.
    for (size_t i = 0; i < light_count(); i++) {
        cmnd_unregister(light_name(i));
    }

    esp_err_t err = light_shutdown();
    ESP_LOGI(TAG, "Light adapter shutdown");
    return err;
}

static void tele_light(const char *tele_id, cJSON *json_root) {
    (void)tele_id;

    // "lights" lists the names of the flat per-light objects below, so a UI polling /tele
    // can tell which top-level keys are lights without the state itself being nested.
    cJSON *names = cJSON_CreateArray();

    for (size_t i = 0; i < light_count(); i++) {
        light_caps_t caps;
        light_state_t state;
        if (!light_get_caps(i, &caps) || !light_get_state(i, &state)) {
            continue;
        }

        cJSON *obj = cJSON_CreateObject();

        cJSON_AddBoolToObject(obj, "on", state.on);
        if (!caps.is_switch) {
            cJSON_AddNumberToObject(obj, "v", state.brightness);
        }
        if (state.effect) {
            cJSON_AddStringToObject(obj, "effect", state.effect);
        }
        if (caps.has_cct) {
            cJSON_AddNumberToObject(obj, "cct", state.cct);
        }

        if (caps.has_color) {
            cJSON_AddNumberToObject(obj, "r", state.color.r);
            cJSON_AddNumberToObject(obj, "g", state.color.g);
            cJSON_AddNumberToObject(obj, "b", state.color.b);
        }

        // Per-channel values alongside cct/r/g/b, not instead of - HA's color_temp_template
        // still reads cct. A simple UI can render one control per physical channel just from
        // which keys are present: "c" -> cold-white button, "w" -> warm-white button.
        if (caps.has_cold_white) {
            cJSON_AddNumberToObject(obj, "c", state.color.c);
        }
        if (caps.has_warm_white) {
            cJSON_AddNumberToObject(obj, "w", state.color.w);
        }

        cJSON_AddItemToObject(json_root, light_name(i), obj);
        cJSON_AddItemToArray(names, cJSON_CreateString(light_name(i)));
    }

    cJSON_AddItemToObject(json_root, "lights", names);
}

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY

static void light_ha_build(cJSON *payload, const char *sanitized_name) {
    int8_t idx = light_index_by_name(sanitized_name);
    light_caps_t caps = {0};
    bool found = idx >= 0 && light_get_caps((size_t)idx, &caps);
    char cmd_buf[400];
    char buf[192];

    cJSON_AddStringToObject(payload, "schema", "template");

    snprintf(cmd_buf, sizeof(cmd_buf),
             "{\"%s\":{ "
             "{%% if hue is defined %%}\"h\":{{ hue }},{%% endif %%}"
             "{%% if sat is defined %%}\"s\":{{ sat }},{%% endif %%}"
             "{%% if brightness is defined %%}\"v\":{{ (brightness / 255 * 100) | round }},{%% "
             "endif %%}"
             "{%% if color_temp is defined %%}\"cct\":{{ ((color_temp - %d) / (%d - %d) * 100) | "
             "round }},{%% endif %%}"
             "{%% if effect is defined %%}\"effect\":\"{{ effect }}\",{%% endif %%}"
             "\"on\":true}}",
             sanitized_name, LIGHT_KELVIN_MIN, LIGHT_KELVIN_MAX, LIGHT_KELVIN_MIN);

    cJSON_AddStringToObject(payload, "command_on_template", cmd_buf);

    snprintf(buf, sizeof(buf), "{\"%s\":{\"on\":false}}", sanitized_name);
    cJSON_AddStringToObject(payload, "command_off_template", buf);

    snprintf(buf, sizeof(buf), "{%% if value_json.%s.on %%}on{%% else %%}off{%% endif %%}",
             sanitized_name);
    cJSON_AddStringToObject(payload, "state_template", buf);

    if (found && !caps.is_switch) {
        snprintf(buf, sizeof(buf), "{{ (value_json.%s.v / 100 * 255) | round }}", sanitized_name);
        cJSON_AddStringToObject(payload, "brightness_template", buf);
    }

    if (found && caps.has_color) {
        snprintf(buf, sizeof(buf), "{{ value_json.%s.r }}", sanitized_name);
        cJSON_AddStringToObject(payload, "red_template", buf);
        snprintf(buf, sizeof(buf), "{{ value_json.%s.g }}", sanitized_name);
        cJSON_AddStringToObject(payload, "green_template", buf);
        snprintf(buf, sizeof(buf), "{{ value_json.%s.b }}", sanitized_name);
        cJSON_AddStringToObject(payload, "blue_template", buf);
    }

    if (found && caps.has_cct) {
        cJSON_AddBoolToObject(payload, "color_temp_kelvin", true);
        cJSON_AddNumberToObject(payload, "min_kelvin", LIGHT_KELVIN_MIN);
        cJSON_AddNumberToObject(payload, "max_kelvin", LIGHT_KELVIN_MAX);
        snprintf(buf, sizeof(buf), "{{ (value_json.%s.cct / 100 * (%d - %d) + %d) | round }}",
                 sanitized_name, LIGHT_KELVIN_MAX, LIGHT_KELVIN_MIN, LIGHT_KELVIN_MIN);
        cJSON_AddStringToObject(payload, "color_temp_template", buf);
    }

    if (found && caps.has_effects) {
        cJSON *effect_list = cJSON_CreateArray();
        for (size_t i = 0; i < light_effect_count(); i++) {
            cJSON_AddItemToArray(effect_list, cJSON_CreateString(light_effect_name(i)));
        }
        cJSON_AddItemToObject(payload, "effect_list", effect_list);

        snprintf(buf, sizeof(buf), "{{ value_json.%s.effect }}", sanitized_name);
        cJSON_AddStringToObject(payload, "effect_template", buf);
    }

    cJSON_DeleteItemFromObject(payload, "val_tpl");
}

#define HA_ENTITY_ENTRY(light_name)                                                                \
    {.type = HA_LIGHT, .name = light_name, .custom_builder = light_ha_build},

static const ha_metadata_t light_ha_metadata = {
    .magic = HA_METADATA_MAGIC, .entities = {HA_ENTITY_LIST{.type = HA_ENTITY_NONE}}};
#undef HA_ENTITY_ENTRY
#endif

supervisor_platform_adapter_t light_adapter = {
    .name = "light",
    .init = light_adapter_init,
    .shutdown = light_adapter_shutdown,
    .on_interval = light_adapter_on_interval,
    .tele_group = (const tele_entry_t[]){{"light", tele_light}, {NULL, NULL}},
    .cmnd_group = NULL, // registered dynamically per light in light_adapter_init
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    .metadata = &light_ha_metadata,
#endif
};
