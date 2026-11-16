#include "thread_coap_server.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/coap.h"
#include "openthread/ip6.h"
#include "openthread/message.h"
#include "openthread/thread.h"

#include "cmnd.h"

#define TAG "cikon:thread:coap"

// A command is a short JSON object; a longer request is refused
#define COAP_PAYLOAD_MAX 256

static otCoapResource s_cmnd_resource;

static void thread_coap_server_send_response(otMessage *request, const otMessageInfo *info,
                                             otCoapCode code, const char *payload) {
    otInstance *instance = esp_openthread_get_instance();
    otMessage *response = otCoapNewMessage(instance, NULL);
    if (response == NULL) {
        return;
    }

    otError err = otCoapMessageInitResponse(response, request, OT_COAP_TYPE_ACKNOWLEDGMENT, code);
    if (err == OT_ERROR_NONE && payload != NULL) {
        err = otCoapMessageSetPayloadMarker(response);
        if (err == OT_ERROR_NONE) {
            err = otMessageAppend(response, payload, (uint16_t)strlen(payload));
        }
    }
    if (err == OT_ERROR_NONE) {
        err = otCoapSendResponse(instance, response, info);
    }
    if (err != OT_ERROR_NONE) {
        otMessageFree(response);
    }
}

static void thread_coap_server_cmnd_handler(void *context, otMessage *message,
                                            const otMessageInfo *info) {
    (void)context;

    otCoapCode code = otCoapMessageGetCode(message);
    if (code != OT_COAP_CODE_POST && code != OT_COAP_CODE_PUT) {
        return;
    }

    // Only a confirmable request is answered: a multicast one is non-confirmable
    bool confirmable = otCoapMessageGetType(message) == OT_COAP_TYPE_CONFIRMABLE;

    uint16_t offset = otMessageGetOffset(message);
    uint16_t length = otMessageGetLength(message) - offset;

    if (length == 0 || length >= COAP_PAYLOAD_MAX) {
        if (confirmable) {
            thread_coap_server_send_response(message, info, OT_COAP_CODE_BAD_REQUEST, NULL);
        }
        return;
    }

    char payload[COAP_PAYLOAD_MAX];
    otMessageRead(message, offset, payload, length);
    payload[length] = '\0';

    cmnd_process_json(payload);

    if (confirmable) {
        char ml_eid[OT_IP6_ADDRESS_STRING_SIZE];
        otIp6AddressToString(otThreadGetMeshLocalEid(esp_openthread_get_instance()), ml_eid,
                             sizeof(ml_eid));

        char response[sizeof("{\"ok\":true,\"ml_eid\":\"\"}") + OT_IP6_ADDRESS_STRING_SIZE];
        snprintf(response, sizeof(response), "{\"ok\":true,\"ml_eid\":\"%s\"}", ml_eid);
        thread_coap_server_send_response(message, info, OT_COAP_CODE_CHANGED, response);
    }
}

void thread_coap_server_start(void) {
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();

    s_cmnd_resource.mUriPath = "cmnd";
    s_cmnd_resource.mHandler = thread_coap_server_cmnd_handler;

    otError err = otCoapStart(instance, OT_DEFAULT_COAP_PORT);
    if (err == OT_ERROR_NONE) {
        otCoapAddResource(instance, &s_cmnd_resource);
        ESP_LOGI(TAG, "CoAP server on port %d, resource /cmnd", OT_DEFAULT_COAP_PORT);
    } else {
        ESP_LOGE(TAG, "CoAP start failed: %s", otThreadErrorToString(err));
    }
    esp_openthread_lock_release();
}
