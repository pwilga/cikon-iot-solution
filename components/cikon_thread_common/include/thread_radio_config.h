#pragma once

#include "esp_openthread_types.h"

#if CONFIG_OPENTHREAD_RADIO_SPINEL_UART
#define THREAD_DEFAULT_RADIO_CONFIG()                          \
    {                                                          \
        .radio_mode = RADIO_MODE_UART_RCP,                     \
        .radio_uart_config = {                                 \
            .port = CONFIG_THREAD_RADIO_RCP_UART_PORT,         \
            .uart_config = {                                   \
                .baud_rate = CONFIG_THREAD_RADIO_RCP_BAUD,     \
                .data_bits = UART_DATA_8_BITS,                 \
                .parity = UART_PARITY_DISABLE,                 \
                .stop_bits = UART_STOP_BITS_1,                 \
                .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,         \
                .source_clk = UART_SCLK_DEFAULT,               \
            },                                                 \
            .rx_pin = CONFIG_THREAD_RADIO_RCP_RX_PIN,          \
            .tx_pin = CONFIG_THREAD_RADIO_RCP_TX_PIN,          \
        },                                                     \
    }
#else
#define THREAD_DEFAULT_RADIO_CONFIG() \
    {                                 \
        .radio_mode = RADIO_MODE_NATIVE, \
    }
#endif

#define THREAD_DEFAULT_OT_CONFIG()                                  \
    {                                                               \
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),             \
        .platform_config = {                                        \
            .radio_config = THREAD_DEFAULT_RADIO_CONFIG(),          \
            .host_config = {                                        \
                .host_connection_mode = HOST_CONNECTION_MODE_NONE,  \
            },                                                      \
            .port_config = {                                        \
                .storage_partition_name = "nvs",                    \
                .netif_queue_size = 10,                             \
                .task_queue_size = 10,                              \
            },                                                      \
        },                                                          \
    }
