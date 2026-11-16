#pragma once

/**
 * Starts the CoAP server: one resource, `cmnd`, takes the same JSON as the MQTT command topic. A
 * confirmable request is answered with the device's mesh-local address (`{"ok":true,"ml_eid":..}`),
 * a non-confirmable one (multicast) gets no answer. Takes the OpenThread lock itself, so call it
 * without.
 */
void thread_coap_server_start(void);
