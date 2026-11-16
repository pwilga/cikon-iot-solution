#pragma once

#include "esp_err.h"

/**
 * Sends a command to another Thread device by name, over CoAP. Returns right away - the answer
 * comes in OpenThread callbacks, so it is safe to call from a command handler or a button
 * callback. Takes the OpenThread lock itself, so call it without.
 *
 * The device is tried at its mesh-local address kept in NVS, and when that does not answer, at the
 * address DNS gives for the name. Every answer carries the mesh-local address, which is kept.
 *
 * @param name  Host name of the device, as it registers with SRP
 * @param json  Command in the form of the MQTT command topic, e.g. {"onboard_led":"toggle"}
 *
 * @retval ESP_ERR_INVALID_STATE  A previous command is still on its way
 * @retval ESP_ERR_INVALID_ARG    Name or command empty or too long
 */
esp_err_t thread_coap_client_send(const char *name, const char *json);

/** Command handler of `send`: {"to": "<name>", "cmnd": {<command>}} */
void thread_coap_client_send_handler(const char *args_json_str);
