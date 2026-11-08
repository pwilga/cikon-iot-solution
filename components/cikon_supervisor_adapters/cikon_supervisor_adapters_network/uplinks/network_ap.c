#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"

#include "config_manager.h"
#include "http_server.h"
#include "json_parser.h"
#include "network_ap.h"
#include "wifi.h"

// Local service access point next to the uplink. Not an uplink itself: it only adds an interface
// the already running services answer on.

#define TAG "cikon:network:ap"

#define NETWORK_AP_SCAN_MAX 20     // networks listed by GET /scan
#define NETWORK_AP_SCAN_CACHE_S 30 // GET /scan within this time returns the last results

#define NETWORK_AP_DNS_PORT 53
#define NETWORK_AP_DNS_MAX_LEN 512   // plain DNS over UDP never goes above it
#define NETWORK_AP_DNS_HEADER_LEN 12 // id, flags, 4 counters
#define NETWORK_AP_DNS_ANSWER_LEN 16 // name pointer, type, class, TTL, length, IPv4
#define NETWORK_AP_DNS_TTL_S 60

// Only what the portal needs from wifi_ap_record_t (35 B instead of ~90 B per network)
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth;
} network_ap_scan_entry_t;

// Last scan results: a scan blocks HTTP and pauses STA attempts for ~3 s, so it runs at most once
// per NETWORK_AP_SCAN_CACHE_S no matter how often /scan is called
static network_ap_scan_entry_t scan_entries[NETWORK_AP_SCAN_MAX];
static uint8_t scan_entry_count = 0;
static int64_t scan_time_us = 0; // 0 = no scan yet

// DNS runs on the raw lwIP API: no task, no socket (the socket limit is already nearly used up).
// The pcb and the packet buffer are only touched in the lwIP thread
static struct udp_pcb *dns_pcb = NULL;
static esp_ip4_addr_t dns_ap_ip;
static uint8_t dns_packet[NETWORK_AP_DNS_MAX_LEN];

static void network_ap_handler(const char *args_json_str) {
    logic_state_t state = json_str_as_logic_state(args_json_str);

    if (state == STATE_TOGGLE) {
        wifi_mode_t mode = WIFI_MODE_NULL;
        esp_wifi_get_mode(&mode);
        state = (mode == WIFI_MODE_APSTA) ? STATE_OFF : STATE_ON;
    }

    if (state == STATE_ON) {
        wifi_ap_enable();
    } else {
        wifi_ap_disable();
    }
}

const command_entry_t network_ap_commands[] = {
    {"ap", "Control WiFi access point next to the uplink (on/off)", network_ap_handler},
    {NULL, NULL, NULL}};

// GET /scan for the WiFi setup portal: {"saved": "<ssid>", "aps": [{"ssid", "rssi", "auth"}]}
// Only networks STA can join (WPA2 or better). The portal itself sorts by rssi
static void network_ap_scan_json(cJSON *root) {
    int64_t now_us = esp_timer_get_time();
    int64_t age_s = (now_us - scan_time_us) / 1000000;

    if (scan_time_us == 0 || age_s >= NETWORK_AP_SCAN_CACHE_S) {
        wifi_ap_record_t records[NETWORK_AP_SCAN_MAX];
        uint16_t count = wifi_scan(records, NETWORK_AP_SCAN_MAX);

        scan_entry_count = 0;
        for (uint16_t i = 0; i < count; i++) {
            // STA accepts WPA2 at least - weaker networks would only fail to connect
            if (records[i].authmode < WIFI_AUTH_WPA2_PSK) {
                continue;
            }
            network_ap_scan_entry_t *entry = &scan_entries[scan_entry_count++];
            strlcpy(entry->ssid, (const char *)records[i].ssid, sizeof(entry->ssid));
            entry->rssi = records[i].rssi;
            entry->auth = records[i].authmode;
        }
        scan_time_us = now_us;
    } else {
        ESP_LOGI(TAG, "Scan skipped, returning results from %llds ago", age_s);
    }

    cJSON_AddStringToObject(root, "saved", config_get()->wifi_ssid);

    cJSON *aps = cJSON_AddArrayToObject(root, "aps");
    for (uint8_t i = 0; i < scan_entry_count; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", scan_entries[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", scan_entries[i].rssi);
        cJSON_AddNumberToObject(ap, "auth", scan_entries[i].auth);
        cJSON_AddItemToArray(aps, ap);
    }
}

void network_ap_http_register(void) { http_register_json_get("/scan", network_ap_scan_json); }

// One query in, one reply out: an A question gets the AP address, any other (AAAA, HTTPS) an empty
// answer, so the phone stays on IPv4. Runs in the lwIP thread
static void network_ap_dns_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                                const ip_addr_t *addr, u16_t port) {

    // Room for the answer stays at the end of the buffer
    uint16_t len =
        pbuf_copy_partial(p, dns_packet, sizeof(dns_packet) - NETWORK_AP_DNS_ANSWER_LEN, 0);
    pbuf_free(p);

    // Standard query (QR and opcode 0) with exactly one question
    if (len < NETWORK_AP_DNS_HEADER_LEN || (dns_packet[2] & 0xF8) != 0 || dns_packet[4] != 0 ||
        dns_packet[5] != 1) {
        return;
    }

    // Skip the question name: length-prefixed labels up to a zero byte
    uint16_t pos = NETWORK_AP_DNS_HEADER_LEN;
    while (pos < len && dns_packet[pos] != 0) {
        if (dns_packet[pos] & 0xC0) {
            return; // compression pointer, never used in a question
        }
        pos += dns_packet[pos] + 1;
    }

    // Zero byte, type and class must fit
    if (pos + 5 > len) {
        return;
    }

    bool type_a = dns_packet[pos + 1] == 0 && dns_packet[pos + 2] == 1 &&
                  dns_packet[pos + 3] == 0 && dns_packet[pos + 4] == 1;
    uint16_t reply_len = pos + 5; // what follows the question (EDNS) is dropped

    dns_packet[2] |= 0x80; // response, keeps "recursion desired"
    dns_packet[3] = 0x80;  // recursion available, no error
    dns_packet[6] = 0;
    dns_packet[7] = type_a ? 1 : 0;
    memset(&dns_packet[8], 0, 4); // no authority, no additional records

    if (type_a) {
        uint8_t *answer = &dns_packet[reply_len];
        const uint8_t answer_head[] = {0xC0, NETWORK_AP_DNS_HEADER_LEN, // name = the question's
                                       0,    1,                         // type A
                                       0,    1,                         // class IN
                                       0,    0,
                                       0,    NETWORK_AP_DNS_TTL_S, // TTL
                                       0,    4};                   // IPv4 length
        memcpy(answer, answer_head, sizeof(answer_head));
        memcpy(answer + sizeof(answer_head), &dns_ap_ip.addr, 4); // already in network order
        reply_len += NETWORK_AP_DNS_ANSWER_LEN;
    }

    struct pbuf *reply = pbuf_alloc(PBUF_TRANSPORT, reply_len, PBUF_RAM);
    if (reply == NULL) {
        return;
    }
    pbuf_take(reply, dns_packet, reply_len);
    udp_sendto(pcb, reply, addr, port);
    pbuf_free(reply);
}

static void network_ap_dns_open(void *arg) {

    if (dns_pcb) {
        return;
    }

    dns_pcb = udp_new();
    if (dns_pcb == NULL) {
        ESP_LOGE(TAG, "DNS: no memory for the UDP pcb");
        return;
    }

    ip_addr_t bind_ip = IPADDR4_INIT(dns_ap_ip.addr);
    err_t err = udp_bind(dns_pcb, &bind_ip, NETWORK_AP_DNS_PORT);
    if (err != ERR_OK) {
        ESP_LOGE(TAG, "DNS: bind to port %d failed: %s", NETWORK_AP_DNS_PORT,
                 strerror(err_to_errno(err)));
        udp_remove(dns_pcb);
        dns_pcb = NULL;
        return;
    }

    udp_recv(dns_pcb, network_ap_dns_recv, NULL);
    ESP_LOGI(TAG, "DNS started, every name points to " IPSTR, IP2STR(&dns_ap_ip));
}

static void network_ap_dns_close(void *arg) {

    if (dns_pcb == NULL) {
        return;
    }

    udp_remove(dns_pcb);
    dns_pcb = NULL;
    ESP_LOGI(TAG, "DNS stopped");
}

// lwIP calls go through its own thread: start and stop queue up there in order, so a quick
// ap off / ap on can't race
void network_ap_dns_start(esp_ip4_addr_t ap_ip) {
    dns_ap_ip = ap_ip;
    if (tcpip_callback(network_ap_dns_open, NULL) != ERR_OK) {
        ESP_LOGE(TAG, "DNS: start not queued");
    }
}

void network_ap_dns_stop(void) {
    if (tcpip_callback(network_ap_dns_close, NULL) != ERR_OK) {
        ESP_LOGE(TAG, "DNS: stop not queued");
    }
}
