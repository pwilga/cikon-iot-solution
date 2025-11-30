#ifndef RF433_RECEIVER_H
#define RF433_RECEIVER_H

#include <stdint.h>

#include "esp_event_base.h"
#include "soc/gpio_num.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(RF433_EVENTS);

typedef enum {
    RF433_CODE_RECEIVED,
} rf433_event_id_t;

typedef struct {
    uint32_t code;
    uint8_t bits;
} rf433_event_data_t;

void rf433_receiver_configure(gpio_num_t rx_pin);
void rf433_receiver_init(void);
void rf433_receiver_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // RF433_RECEIVER_H
