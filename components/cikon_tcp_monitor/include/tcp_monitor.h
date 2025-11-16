#ifndef TCP_MONITOR_H
#define TCP_MONITOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void tcp_monitor_configure(uint16_t port);

void tcp_monitor_init(void);
void tcp_monitor_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // TCP_MONITOR_H
