#include "thread_coap_client.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "nvs.h"
#include "openthread/coap.h"
#include "openthread/dns_client.h"
#include "openthread/ip6.h"
#include "openthread/message.h"

#include "json_parser.h"

#define TAG "cikon:thread:coap_client"

#define SEND_NVS_NAMESPACE "send"
#define SEND_JSON_MAX 256
#define SEND_HOST_SUFFIX ".default.service.arpa"

// The server answers {"ok":true,"ml_eid":"<address>"}, about 60 characters
#define COAP_RESPONSE_MAX 80

#define DNS_RESPONSE_TIMEOUT_MS 1500
#define DNS_MAX_TX_ATTEMPTS 2

// A device that is there answers at once: a short wait, one retransmission, instead of the
// 2 s and four retransmissions of the standard
static const otCoapTxParameters s_coap_params = {
    .mAckTimeout = 1000,
    .mAckRandomFactorNumerator = 1,
    .mAckRandomFactorDenominator = 1,
    .mMaxRetransmit = 1,
};

// The one command on its way. Everything below runs with the OpenThread lock held
static struct {
    bool busy;
    char name[NVS_KEY_NAME_MAX_SIZE];
    char json[SEND_JSON_MAX];
    char host[NVS_KEY_NAME_MAX_SIZE + sizeof(SEND_HOST_SUFFIX)];
    bool via_dns; // the next step failing means the device is not there
} s_req;

static void thread_coap_client_request_finish(void) { s_req.busy = false; }

static void thread_coap_client_nvs_key(char *key, size_t size) {
    snprintf(key, size, "%s", s_req.name);
}

static bool thread_coap_client_ml_eid_load(otIp6Address *address) {
    nvs_handle_t nvs;
    if (nvs_open(SEND_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    char key[NVS_KEY_NAME_MAX_SIZE], text[OT_IP6_ADDRESS_STRING_SIZE];
    size_t size = sizeof(text);
    thread_coap_client_nvs_key(key, sizeof(key));
    bool ok = nvs_get_str(nvs, key, text, &size) == ESP_OK &&
              otIp6AddressFromString(text, address) == OT_ERROR_NONE;
    nvs_close(nvs);
    return ok;
}

static void thread_coap_client_ml_eid_save(const char *ml_eid) {
    nvs_handle_t nvs;
    if (nvs_open(SEND_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    char key[NVS_KEY_NAME_MAX_SIZE], text[OT_IP6_ADDRESS_STRING_SIZE] = "";
    size_t size = sizeof(text);
    thread_coap_client_nvs_key(key, sizeof(key));
    nvs_get_str(nvs, key, text, &size);
    if (strcmp(text, ml_eid) != 0 && nvs_set_str(nvs, key, ml_eid) == ESP_OK) {
        nvs_commit(nvs);
        ESP_LOGI(TAG, "%s: mesh-local address kept: %s", s_req.name, ml_eid);
    }
    nvs_close(nvs);
}

static void thread_coap_client_dns_resolve(void);

static void thread_coap_client_response(void *context, otMessage *message,
                                        const otMessageInfo *info, otError result) {
    (void)context;
    (void)info;

    if (result != OT_ERROR_NONE) {
        if (s_req.via_dns) {
            ESP_LOGW(TAG, "%s: no answer at the address from DNS", s_req.name);
            thread_coap_client_request_finish();
        } else {
            thread_coap_client_dns_resolve();
        }
        return;
    }

    // {"ok":true,"ml_eid":"fd.."}
    uint16_t offset = otMessageGetOffset(message);
    uint16_t length = otMessageGetLength(message) - offset;
    char payload[COAP_RESPONSE_MAX];
    if (length > 0 && length < sizeof(payload)) {
        otMessageRead(message, offset, payload, length);
        payload[length] = '\0';
        cJSON *reply = cJSON_Parse(payload);
        const cJSON *ml_eid = cJSON_GetObjectItemCaseSensitive(reply, "ml_eid");
        if (cJSON_IsString(ml_eid)) {
            thread_coap_client_ml_eid_save(ml_eid->valuestring);
        }
        cJSON_Delete(reply);
    }
    ESP_LOGI(TAG, "%s: sent", s_req.name);
    thread_coap_client_request_finish();
}

static void thread_coap_client_send_to(const otIp6Address *address) {
    otInstance *instance = esp_openthread_get_instance();
    otMessage *message = otCoapNewMessage(instance, NULL);
    if (message == NULL) {
        ESP_LOGE(TAG, "%s: no message buffers", s_req.name);
        thread_coap_client_request_finish();
        return;
    }

    otError err = otCoapMessageInit(message, OT_COAP_TYPE_CONFIRMABLE, OT_COAP_CODE_POST);
    if (err == OT_ERROR_NONE) {
        otCoapMessageGenerateToken(message, OT_COAP_DEFAULT_TOKEN_LENGTH);
        err = otCoapMessageAppendUriPathOptions(message, "cmnd");
    }
    if (err == OT_ERROR_NONE) {
        err = otCoapMessageSetPayloadMarker(message);
    }
    if (err == OT_ERROR_NONE) {
        err = otMessageAppend(message, s_req.json, (uint16_t)strlen(s_req.json));
    }

    otMessageInfo info = {0};
    info.mPeerAddr = *address;
    info.mPeerPort = OT_DEFAULT_COAP_PORT;
    if (err == OT_ERROR_NONE) {
        err = otCoapSendRequestWithParameters(instance, message, &info, thread_coap_client_response,
                                              NULL, &s_coap_params);
    }
    if (err != OT_ERROR_NONE) {
        otMessageFree(message);
        ESP_LOGE(TAG, "%s: send failed: %s", s_req.name, otThreadErrorToString(err));
        thread_coap_client_request_finish();
    }
}

static void thread_coap_client_dns_response(otError error, const otDnsAddressResponse *response,
                                            void *context) {
    (void)context;

    otIp6Address address;
    if (error != OT_ERROR_NONE ||
        otDnsAddressResponseGetAddress(response, 0, &address, NULL) != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "%s: DNS has no address", s_req.name);
        thread_coap_client_request_finish();
        return;
    }
    s_req.via_dns = true;
    thread_coap_client_send_to(&address);
}

static void thread_coap_client_dns_resolve(void) {
    otInstance *instance = esp_openthread_get_instance();

    otDnsQueryConfig config = *otDnsClientGetDefaultConfig(instance);
    config.mResponseTimeout = DNS_RESPONSE_TIMEOUT_MS;
    config.mMaxTxAttempts = DNS_MAX_TX_ATTEMPTS;

    snprintf(s_req.host, sizeof(s_req.host), "%s" SEND_HOST_SUFFIX, s_req.name);
    otError err = otDnsClientResolveAddress(instance, s_req.host, thread_coap_client_dns_response,
                                            NULL, &config);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "%s: DNS query failed: %s", s_req.name, otThreadErrorToString(err));
        thread_coap_client_request_finish();
    }
}

esp_err_t thread_coap_client_send(const char *name, const char *json) {
    if (name == NULL || json == NULL || name[0] == '\0' || strlen(name) >= NVS_KEY_NAME_MAX_SIZE ||
        strlen(json) >= SEND_JSON_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    if (s_req.busy) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_req.busy = true;
        s_req.via_dns = false;
        snprintf(s_req.name, sizeof(s_req.name), "%s", name);
        snprintf(s_req.json, sizeof(s_req.json), "%s", json);

        otError start_err = otCoapStart(esp_openthread_get_instance(), OT_DEFAULT_COAP_PORT);
        if (start_err != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "CoAP start failed: %s", otThreadErrorToString(start_err));
        }

        otIp6Address ml_eid;
        if (thread_coap_client_ml_eid_load(&ml_eid)) {
            thread_coap_client_send_to(&ml_eid);
        } else {
            thread_coap_client_dns_resolve();
        }
    }

    esp_openthread_lock_release();
    return err;
}

void thread_coap_client_send_handler(const char *args_json_str) {
    cJSON *args = json_str_as_object(args_json_str);
    const cJSON *to = cJSON_GetObjectItemCaseSensitive(args, "to");
    const cJSON *cmnd = cJSON_GetObjectItemCaseSensitive(args, "cmnd");

    if (!cJSON_IsString(to) || !cJSON_IsObject(cmnd)) {
        ESP_LOGW(TAG, "send:needs {\"to\":\"<name>\",\"cmnd\":{<command>}}");
        cJSON_Delete(args);
        return;
    }

    char *json = cJSON_PrintUnformatted(cmnd);
    esp_err_t err = thread_coap_client_send(to->valuestring, json ? json : "");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send:%s", esp_err_to_name(err));
    }
    free(json);
    cJSON_Delete(args);
}
