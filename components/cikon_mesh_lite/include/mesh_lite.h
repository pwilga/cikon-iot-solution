#pragma once

#include "esp_err.h"
#include "esp_mesh_lite.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mesh_lite_init(void);
esp_err_t mesh_lite_shutdown(void);

#ifdef __cplusplus
}
#endif
