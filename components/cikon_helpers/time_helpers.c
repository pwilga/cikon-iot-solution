#include "time_helpers.h"

#include <string.h>
#include <time.h>

void format_current_time(char *buf, size_t buflen, const char *format) {
    if (!buf || buflen == 0) {
        return;
    }

    if (!format) {
        format = TIME_FORMAT_DEFAULT;
    }

    time_t now_sec = 0;
    time(&now_sec);

    if (now_sec < 1000000000) { // Before ~2001 - time not set
        strncpy(buf, "Time not set", buflen - 1);
        buf[buflen - 1] = '\0';
        return;
    }

    struct tm tm_now = {0};
    localtime_r(&now_sec, &tm_now);

    strftime(buf, buflen, format, &tm_now);
}
