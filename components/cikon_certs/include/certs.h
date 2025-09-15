#ifndef CERT_SYMBOL_H
#define CERT_SYMBOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char* get_ca_pem_start(void);
size_t get_ca_pem_size(void);

const char* get_client_pem_start(void);
size_t get_client_pem_size(void);

const char* get_client_key_start(void);
size_t get_client_key_size(void);

bool certs_available(void);

#ifdef __cplusplus
}
#endif

#endif // CERT_SYMBOL_H