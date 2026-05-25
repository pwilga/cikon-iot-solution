#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIME_FORMAT_DEFAULT "%Y-%m-%d %H:%M:%S"

/**
 * @brief Format current system time to string
 *
 * @param[out] buf Output buffer for formatted time string
 * @param[in] buflen Size of output buffer
 * @param[in] format strftime format string (e.g., "%Y-%m-%d %H:%M:%S"), or NULL for default
 *
 * @note If time is not set (before SNTP sync), outputs "Time not set"
 */
void format_current_time(char *buf, size_t buflen, const char *format);

#ifdef __cplusplus
}
#endif
