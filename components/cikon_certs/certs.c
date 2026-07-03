#include "certs.h"
#include <stdio.h>
#include <stdlib.h>

static uint8_t *s_ca = NULL, *s_cert = NULL, *s_key = NULL;
static size_t s_ca_len = 0, s_cert_len = 0, s_key_len = 0;

static uint8_t *load_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size_t len = (size_t)ftell(f);
    rewind(f);
    uint8_t *buf = malloc(len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    fread(buf, 1, len, f);
    buf[len] = '\0'; /* mbedtls requires null-terminated PEM */
    fclose(f);
    *out_len = len + 1;
    return buf;
}

static void certs_load(void) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", CONFIG_CERTS_DIR, CONFIG_CERT_CA_FILENAME);
    s_ca = load_file(path, &s_ca_len);
    snprintf(path, sizeof(path), "%s/%s", CONFIG_CERTS_DIR, CONFIG_CERT_CLIENT_FILENAME);
    s_cert = load_file(path, &s_cert_len);
    snprintf(path, sizeof(path), "%s/%s", CONFIG_CERTS_DIR, CONFIG_CERT_CLIENT_KEY_FILENAME);
    s_key = load_file(path, &s_key_len);
}

static void ensure_loaded(void) {
    if (!s_ca && !s_cert && !s_key)
        certs_load();
}

bool certs_available(void) {
    ensure_loaded();
    return s_ca != NULL && s_cert != NULL && s_key != NULL;
}

const char *get_ca_pem_start(void) {
    ensure_loaded();
    return (const char *)s_ca;
}
size_t get_ca_pem_size(void) {
    ensure_loaded();
    return s_ca_len;
}
const char *get_client_pem_start(void) {
    ensure_loaded();
    return (const char *)s_cert;
}
size_t get_client_pem_size(void) {
    ensure_loaded();
    return s_cert_len;
}
const char *get_client_key_start(void) {
    ensure_loaded();
    return (const char *)s_key;
}
size_t get_client_key_size(void) {
    ensure_loaded();
    return s_key_len;
}
