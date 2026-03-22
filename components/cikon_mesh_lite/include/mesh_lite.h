#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t mesh_id;
    const char *sta_ssid;
    const char *sta_password;
    const char *ap_ssid;
    const char *ap_password;
} mesh_lite_config_t;

void mesh_lite_configure(const mesh_lite_config_t *config);
esp_err_t mesh_lite_init(void);
esp_err_t mesh_lite_shutdown(void);
bool is_mesh_root_node(void);
int mesh_lite_get_child_count(void);
void mesh_log_topology(void);
void mesh_get_info(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
