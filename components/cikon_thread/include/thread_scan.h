#pragma once

#include "cJSON.h"

/**
 * Scans for Thread networks in range. Returns at once, the result goes to the tele. Supervisor
 * command thread_scan.
 */
void thread_scan_handler(const char *args_json_str);

/**
 * Adds "scan": {"state": "scanning"|"done"|"busy"|"failed", "networks": [{"name", "ext_panid",
 * "panid", "channel", "rssi" of the best answer, "answers" from devices of that network}]} to the
 * "thread" tele object, up to THREAD_SCAN_MAX networks, until the next scan. Nothing before the
 * first scan. Busy means that another scan or the joiner was on, scan again.
 */
void thread_scan_tele(cJSON *thread);
