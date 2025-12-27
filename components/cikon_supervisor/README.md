# Cikon Supervisor

Core component managing platform adapters, safe mode, command/telemetry system, and firmware validation.

## Safe Mode

Automatic crash protection mechanism inspired by ESPHome. Detects repeated crashes and enables minimal recovery configuration.

**Activation:** 3 crashes within 90 seconds (configurable via `CONFIG_SUPERVISOR_SAFE_MODE_THRESHOLD`)

**Safe Mode Behavior:**
- Only adapters with `enable_in_safe_mode = true` are initialized
- Typically: WiFi + TCP OTA (port 5555) + TCP Monitor (port 6666)
- Hardware adapters (sensors, actuators) disabled
- Firmware validated immediately (enables instant OTA recovery)

**Auto-Clear:** Boot counter resets after 90s stable operation (configurable via `CONFIG_SUPERVISOR_SAFE_MODE_STABLE_TIME_S`)

**Exit:** Manual restart required (OTA, `cmnd/restart`, physical reset)

**Logs:**
```
Safe mode active: 3 crashes detected
Hardware adapters DISABLED - WiFi/OTA only
Auto-clear after 90s stable operation
```

## Adapter Registration

Register platform adapters in `main.c`:

```c
#include "supervisor.h"
#include "my_adapter.h"

void app_main(void) {
    supervisor_init();
    supervisor_register_adapter(&my_adapter);
    supervisor_platform_init();
}
```

## Adapter Interface

```c
typedef struct {
    void (*init)(void);
    void (*shutdown)(void);
    void (*on_event)(EventBits_t bits);
    void (*on_interval)(supervisor_interval_stage_t stage);
    const tele_entry_t *tele_group;
    const cmnd_entry_t *cmnd_group;
    bool enable_in_safe_mode;  // Set true for essential adapters
} supervisor_platform_adapter_t;
```

## Intervals

Periodic callback stages from 1 second to 12 hours - use `on_interval()` for timed tasks.

## Events

Notify adapters via event group:

```c
supervisor_notify_event(SUPERVISOR_EVENT_PLATFORM_INITIALIZED);
```

Common events:
- `SUPERVISOR_EVENT_PLATFORM_INITIALIZED` - All adapters initialized
- `SUPERVISOR_EVENT_CMND_COMPLETED` - Command execution finished

## Firmware Validation

**Normal Mode:** Firmware validated after 10 seconds (conservative approach)

**Safe Mode:** Firmware validated immediately at init (enables instant OTA recovery)

Prevents OTA rollback on first boot after update.

## Core Commands

- `cmnd/restart` - Restart device
- `cmnd/help` - List all available commands
- `cmnd/setconf` - Set configuration from JSON
- `cmnd/resetconf` - Reset NVS and restart
- `cmnd/onboard_led` - Control onboard LED (on/off/toggle)

## Core Telemetry

- `tele/uptime` - Seconds since boot
- `tele/startup` - Boot timestamp
- `tele/onboard_led` - LED state

## Configuration

`Kconfig` options:
- `CONFIG_SUPERVISOR_SAFE_MODE_THRESHOLD` - Crash count trigger (default: 3)
- `CONFIG_SUPERVISOR_SAFE_MODE_STABLE_TIME_S` - Auto-clear time (default: 90s)
- `CONFIG_SUPERVISOR_TASK_STACK_SIZE` - Task stack size (default: 4096)
- `CONFIG_SUPERVISOR_TASK_PRIORITY` - Task priority (default: 5)
- `CONFIG_SUPERVISOR_QUEUE_LENGTH` - Command queue size (default: 10)
