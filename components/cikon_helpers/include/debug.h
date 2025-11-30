#ifndef DEBUG_H
#define DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

void debug_info_task(void *args);
void debug_print_config_summary();
void debug_print_sys_info();
void debug_print_tasks_summary();

void led_blink_task(void *args);

#ifdef __cplusplus
}
#endif

#endif // DEBUG_H
