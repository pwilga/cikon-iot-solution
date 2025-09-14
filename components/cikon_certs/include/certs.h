#ifndef CERT_SYMBOL_H
#define CERT_SYMBOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const uint8_t* get_ca_pem_start(void);
size_t get_ca_pem_size(void);

const uint8_t* get_client_pem_start(void);
size_t get_client_pem_size(void);

const uint8_t* get_client_key_start(void);
size_t get_client_key_size(void);

#ifdef __cplusplus
}
#endif

#endif // CERT_SYMBOL_H