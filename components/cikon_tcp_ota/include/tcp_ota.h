#ifndef OTA_H
#define OTA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void tcp_ota_configure(uint16_t port);

void tcp_ota_init(void);
void tcp_ota_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // OTA_H