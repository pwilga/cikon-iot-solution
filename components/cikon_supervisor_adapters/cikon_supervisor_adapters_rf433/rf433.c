#include <stdint.h>
#include <sys/types.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"

#include "cmnd.h"
#include "ha.h"
#include "rf433_adapter.h"
#include "rf433_receiver.h"
#include "supervisor.h"
#include "tele.h"

#define TAG "cikon-rf433-adapter"

static uint32_t last_rf_code = 0;

static void rf433_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {

    rf433_event_data_t *rf_event = (rf433_event_data_t *)data;

    ESP_LOGI(TAG, "Received code: 0x%06X (%d bits)", rf_event->code, rf_event->bits);

    switch (rf_event->code) {
    case 0x5447C2:
        ESP_LOGI(TAG, "Sonoff button pressed");
        cmnd_submit("onboard_led", "\"toggle\"");
        break;

    case 0xB9F9C1:
        ESP_LOGI(TAG, "Blue button pressed");
        cmnd_submit("onboard_led", "\"toggle\"");
        break;

    default:
        ESP_LOGW(TAG, "Unknown code: 0x%06X", rf_event->code);
        break;
    }

    last_rf_code = rf_event->code;
    supervisor_notify_event(SUPERVISOR_EVENT_CMND_COMPLETED);
}

static void tele_rf433_code(const char *tele_id, cJSON *json_root) {
    char hexbuf[9];
    snprintf(hexbuf, sizeof(hexbuf), "0x%06" PRIX32, last_rf_code);
    cJSON_AddStringToObject(json_root, tele_id, hexbuf);
}

void rf433_adapter_init(void) {
    esp_event_handler_register(RF433_EVENTS, RF433_CODE_RECEIVED, rf433_event_handler, NULL);
    rf433_receiver_configure(CONFIG_RF433_GPIO_PIN);
    rf433_receiver_init();
    ha_register_entity(HA_SENSOR, "rf433_code", NULL, NULL, NULL);
}

// void rf433_adapter_shutdown(void) {}

static void rf433_adapter_on_event(EventBits_t bits) {}

static void rf433_adapter_on_interval(supervisor_interval_stage_t stage) {}

supervisor_platform_adapter_t rf433_adapter = {
    .init = rf433_adapter_init,
    .shutdown = rf433_receiver_shutdown,
    .on_event = rf433_adapter_on_event,
    .on_interval = rf433_adapter_on_interval,
    .tele_group = (const tele_entry_t[]){{"rf433", tele_rf433_code}, {NULL, NULL}}};
