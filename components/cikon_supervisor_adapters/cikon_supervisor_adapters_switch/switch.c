#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "soc/gpio_num.h"
#include "soc/soc_caps.h"

#include "cJSON.h"

#include "cmnd.h"
#include "config_manager.h"
#include "json_parser.h"
#include "metadata.h"
#include "supervisor.h"
#include "switch_adapter.h"
#include "tele.h"

#define TAG "cikon:adapter:switch"

typedef struct {
    gpio_num_t gpio;
    bool active_level; // Physical level that means "on"
    bool state;        // Cached logical state
    char name[16];
} switch_config_t;

static switch_config_t switches[CONFIG_SWITCH_MAX_COUNT + 1]; // +1 sentinel
static bool switch_initialized = false;

static void switch_parse_list(void) {

    const char *list_str = CONFIG_SWITCH_GPIO_LIST;
    char *str = strdup(list_str);
    char *token = strtok(str, ",");
    int index = 0;

    // Initialize all with sentinel first
    for (int i = 0; i <= CONFIG_SWITCH_MAX_COUNT; i++) {
        switches[i].gpio = GPIO_NUM_NC;
    }

    while (token != NULL && index < CONFIG_SWITCH_MAX_COUNT) {
        // Trim whitespace
        while (*token == ' ')
            token++;

        char *colon = strchr(token, ':');
        if (!colon) {
            ESP_LOGW(TAG, "Invalid format (expected gpio:active_level[:name]): %s", token);
            token = strtok(NULL, ",");
            continue;
        }

        *colon = '\0';
        int gpio = atoi(token);

        char *remainder = colon + 1;
        char *name_colon = strchr(remainder, ':');
        const char *name = NULL;
        if (name_colon) {
            *name_colon = '\0';
            name = name_colon + 1;
        }
        int active_level = atoi(remainder);

        if (gpio < 0 || gpio >= SOC_GPIO_PIN_COUNT) {
            ESP_LOGW(TAG, "Invalid GPIO %d, skipping", gpio);
            token = strtok(NULL, ",");
            continue;
        }

        if (active_level != 0 && active_level != 1) {
            ESP_LOGW(TAG, "Invalid active_level %d for GPIO %d (must be 0 or 1), skipping",
                     active_level, gpio);
            token = strtok(NULL, ",");
            continue;
        }

        switches[index].gpio = (gpio_num_t)gpio;
        switches[index].active_level = (bool)active_level;
        switches[index].state = false;

        if (name && strlen(name) > 0) {
            strncpy(switches[index].name, name, sizeof(switches[index].name) - 1);
            switches[index].name[sizeof(switches[index].name) - 1] = '\0';
            ESP_LOGI(TAG, "Configured switch %d '%s' on GPIO %d (active %s)", index,
                     switches[index].name, gpio, active_level ? "HIGH" : "LOW");
        } else {
            snprintf(switches[index].name, sizeof(switches[index].name), "gpio%d", gpio);
            ESP_LOGI(TAG, "Configured switch %d on GPIO %d (active %s, auto-named '%s')", index,
                     gpio, active_level ? "HIGH" : "LOW", switches[index].name);
        }

        // Canonicalize once here: switch_find_by_name/tele_switch use this name as-is, and
        // cmnd_register (called later, per switch) keeps the pointer forever - not a value
        // it can re-sanitize on every call.
        char *sanitized_name = sanitize(switches[index].name);
        strncpy(switches[index].name, sanitized_name, sizeof(switches[index].name) - 1);
        switches[index].name[sizeof(switches[index].name) - 1] = '\0';
        free(sanitized_name);

        index++;
        token = strtok(NULL, ",");
    }

    if (token != NULL) {
        ESP_LOGE(TAG, "Too many switches configured, max is %d, remaining entries were ignored",
                 CONFIG_SWITCH_MAX_COUNT);
    }

    free(str);
}

static int8_t switch_find_by_name(const char *name) {
    if (!name) {
        return -1;
    }

    for (int i = 0; i < CONFIG_SWITCH_MAX_COUNT; i++) {
        if (switches[i].gpio == GPIO_NUM_NC) {
            break;
        }
        if (strcmp(switches[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

// Persists on every change (not deferred to shutdown) - unlike LED brightness, a switch
// changes rarely enough that NVS wear isn't a concern, and this is what actually survives a
// software reset/watchdog/panic (shutdown() only runs on a clean cmnd/restart).
static void switch_save_state(void) {
    uint32_t packed = 0;
    for (int i = 0; switches[i].gpio != GPIO_NUM_NC; i++) {
        if (switches[i].state) {
            packed |= (1U << i);
        }
    }
    config_set_switch_state(packed);
}

// One cmnd is registered per configured switch (see switch_adapter_init), each pointing at
// its own trampoline below - command_handler_t carries no context, so a single shared
// handler can't tell which switch it was called for. SWITCH_CMND_LIST (injected by
// CMakeLists.txt, sized to match SWITCH_GPIO_LIST) generates exactly as many trampolines as
// there are configured switches.
#ifndef SWITCH_CMND_LIST
#define SWITCH_CMND_LIST // Fallback if CMake didn't inject
#endif

#define X(n)                                                                                       \
    static void switch_cmnd_##n(const char *args_json_str) {                                       \
        logic_state_t state = json_str_as_logic_state(args_json_str);                              \
        bool on = (state == STATE_TOGGLE) ? !switches[n].state : (state == STATE_ON);              \
        ESP_LOGI(TAG, "Setting switch '%s' to %s", switches[n].name, on ? "on" : "off");           \
        switch_set_state(switches[n].name, on);                                                    \
    }
SWITCH_CMND_LIST
#undef X

#define X(n) switch_cmnd_##n,
static const command_handler_t switch_cmnd_trampolines[] = {SWITCH_CMND_LIST};
#undef X

static esp_err_t switch_adapter_init(void) {

    ESP_LOGI(TAG, "Initializing switch adapter");

    if (switch_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    switch_parse_list();

    // Restore before the first gpio_set_direction/gpio_set_level so the pin goes straight to
    // its intended state, instead of "off" first and then corrected - minimizes the glitch
    // window on relays after a reset (the GPIO reset itself briefly de-energizes it either way,
    // that part isn't avoidable in software).
    uint32_t saved_state = config_get()->switch_state;

    for (int i = 0; switches[i].gpio != GPIO_NUM_NC; i++) {
        switch_config_t *out = &switches[i];
        out->state = (saved_state >> i) & 1U;
        ESP_ERROR_CHECK(gpio_reset_pin(out->gpio));
        ESP_ERROR_CHECK(gpio_set_direction(out->gpio, GPIO_MODE_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(out->gpio, out->state == out->active_level ? 1 : 0));
    }

    size_t trampoline_count = sizeof(switch_cmnd_trampolines) / sizeof(switch_cmnd_trampolines[0]);
    for (int i = 0; switches[i].gpio != GPIO_NUM_NC; i++) {
        if ((size_t)i >= trampoline_count) {
            ESP_LOGE(TAG,
                     "No cmnd trampoline for switch '%s' (index %d) - CMake/runtime "
                     "switch count mismatch",
                     switches[i].name, i);
            break;
        }
        cmnd_register(switches[i].name, "Set switch state (on/off/toggle)",
                      switch_cmnd_trampolines[i]);
    }

    switch_initialized = true;
    ESP_LOGI(TAG, "Switch adapter initialized");
    return ESP_OK;
}

static esp_err_t switch_adapter_shutdown(void) {
    if (!switch_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; switches[i].gpio != GPIO_NUM_NC; i++) {
        cmnd_unregister(switches[i].name);
    }

    switch_initialized = false;
    ESP_LOGI(TAG, "Switch adapter shutdown");
    return ESP_OK;
}

void switch_set_state(const char *name, bool on) {
    int8_t idx = switch_find_by_name(name);
    if (idx < 0) {
        ESP_LOGW(TAG, "Switch '%s' not found", name ? name : "(null)");
        return;
    }
    switch_config_t *out = &switches[idx];
    out->state = on;
    ESP_ERROR_CHECK(gpio_set_level(out->gpio, on == out->active_level ? 1 : 0));
    switch_save_state();
}

bool switch_get_state(const char *name) {
    int8_t idx = switch_find_by_name(name);
    if (idx < 0) {
        return false;
    }
    return switches[idx].state;
}

void switch_toggle(const char *name) { switch_set_state(name, !switch_get_state(name)); }

static void tele_switch(const char *tele_id, cJSON *json_root) {
    (void)tele_id;

    for (int i = 0; i < CONFIG_SWITCH_MAX_COUNT; i++) {
        if (switches[i].gpio == GPIO_NUM_NC) {
            break;
        }
        cJSON_AddBoolToObject(json_root, switches[i].name, switches[i].state);
    }
}

#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
#ifndef HA_ENTITY_LIST
#define HA_ENTITY_LIST // Fallback if CMake didn't inject
#endif

#define HA_ENTITY_ENTRY(gpio, switch_name) {.type = HA_SWITCH, .name = switch_name},

static const ha_metadata_t switch_ha_metadata = {
    .magic = HA_METADATA_MAGIC, .entities = {HA_ENTITY_LIST{.type = HA_ENTITY_NONE}}};
#undef HA_ENTITY_ENTRY
#endif

supervisor_platform_adapter_t switch_adapter = {
    .name = "switch",
    .init = switch_adapter_init,
    .shutdown = switch_adapter_shutdown,
    .tele_group = (const tele_entry_t[]){{"switch", tele_switch}, {NULL, NULL}},
    .cmnd_group = NULL, // registered dynamically per switch in switch_adapter_init
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    .metadata = &switch_ha_metadata,
#endif
};
