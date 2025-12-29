# Cikon IoT Platform - Development Guide

## ⚠️ CRITICAL RULE: CHECK EXAMPLES FIRST ⚠️

**BEFORE implementing ANY feature using external components:**

1. **ALWAYS check examples in `managed_components/<component>/examples/` or component documentation**
2. Use the EXACT configuration values from official examples
3. Copy patterns, constants, and initialization sequences verbatim
4. Only search the internet if no examples exist in the component

**NEVER:**
- ❌ Guess configuration values or use arbitrary numbers
- ❌ Make assumptions about API usage without checking examples
- ❌ Modify example values without understanding why

**This is NOT optional.** Using wrong config values wastes hours debugging issues that examples already solved.

---

## Project Structure

This is an ESP-IDF based IoT platform with a supervisor-adapter architecture.

### Repositories
- `idfnode/` - Main ESP32 firmware project
- `cikon-iot-solution/` - Reusable component library

### Core Components Location
- Adapters: `cikon-iot-solution/components/cikon_supervisor_adapters/cikon_supervisor_adapters_*/`
- Supervisor: `cikon-iot-solution/components/cikon_supervisor/`
- Core services: `cikon-iot-solution/components/cikon_*/`

### Architecture Layering (CRITICAL)

**NEVER add network/MQTT/HA dependencies to `cikon_supervisor`!**

Component hierarchy (dependency flow):
```
cikon_supervisor (STANDALONE CORE - no network deps)
    ↓
cikon_helpers (utilities only)
    ↓
cikon_supervisor_adapters_* (hardware abstraction)
    ↓
cikon_supervisor_adapters_inet (NETWORK LAYER - WiFi/MQTT/HA)
```

**Rules:**
- ✅ `cikon_supervisor` = standalone, reusable core (no WiFi, no MQTT, no network)
- ✅ `cikon_helpers` = pure utilities (types, parsing, etc.)
- ✅ Network features (WiFi/MQTT/HA) = `inet` adapter ONLY
- ✅ HA metadata structures = `helpers` (just types, no logic)
- ✅ HA registration/publishing = `inet` adapter (network layer)
- ❌ NEVER add `cikon_mqtt`, `cikon_wifi`, network features to supervisor struct

## Adapter Pattern (MANDATORY READING)

### File Structure
Every adapter MUST have:
```
cikon_supervisor_adapters_<name>/
├── <name>.c              # Implementation
├── include/
│   └── <name>_adapter.h  # Public header (MUST use #pragma once)
├── CMakeLists.txt        # Component build config
├── Kconfig               # Configuration options
└── idf_component.yml     # Dependencies (prefer this over CMakeLists for external deps)
```

### Adapter Interface (supervisor.h)
```c
typedef struct {
    void (*init)(void);
    void (*shutdown)(void);
    void (*on_event)(supervisor_event_type_t event, const char *topic, const void *data);
    void (*on_interval)(supervisor_interval_stage_t stage);
    const tele_entry_t *tele_group;
    const cmnd_entry_t *cmnd_group;
} supervisor_platform_adapter_t;
```

### Implementation Pattern

#### 1. File Organization (MANDATORY ORDER)
Files MUST be structured in this exact order from top to bottom:
1. Includes
2. Defines and static variables
3. Helper functions (if any)
4. Adapter callbacks (`init`, `shutdown`, `on_event`, `on_interval`)
5. Command handlers (`cmnd_<name>_*`)
6. Telemetry functions (`tele_<name>_*`)
7. Command/Telemetry arrays (if multiple entries)
8. **X-Macro/Metadata pattern** (if using HA integration)
9. **Adapter structure** (ALWAYS last)

#### 2. TELE/CMND Array Rules
- **Single entry**: Use compound literal directly in adapter struct
- **Multiple entries**: Declare static array before adapter struct

**Single entry example:**
```c
// No separate array - use compound literal
supervisor_platform_adapter_t adapter = {
    .tele_group = (const tele_entry_t[]){{"ip_address", tele_inet_ip}, {NULL, NULL}},
};
```

**Multiple entries example:**
```c
// Declare array before adapter struct
static const command_entry_t inet_commands[] = {
    {"ap", "Switch to AP mode", set_ap_handler},
    {"sta", "Switch to STA mode", set_sta_handler},
    {"https", "Control HTTPS server", https_handler},
    {NULL, NULL, NULL}
};

supervisor_platform_adapter_t adapter = {
    .cmnd_group = inet_commands,
};
```

#### 3. Adapter Instance (ALWAYS at bottom of .c file)
```c
supervisor_platform_adapter_t <name>_adapter = {
    .init = <name>_adapter_init,
    .shutdown = <name>_adapter_shutdown,
    .on_event = <name>_adapter_on_event,    // or NULL
    .on_interval = <name>_adapter_on_interval, // or NULL
    .tele_group = <tele_array or compound literal>,
    .cmnd_group = <cmnd_array or compound literal>,
};
```

#### 4. Standard Callbacks
```c
static void <name>_adapter_init(void) {
    ESP_LOGI(TAG, "Initializing <name> adapter");  // ⚠️ MANDATORY first log in init()
    // Hardware setup
    // Register HA entities: ha_register_entity()
    // Set initialized flag
    ESP_LOGI(TAG, "<Name> adapter initialized");
}

static void <name>_adapter_shutdown(void) {
    if (!initialized) return;
    ESP_LOGI(TAG, "Shutting down <name> adapter");
    // Cleanup hardware
    // Free resources
    ESP_LOGI(TAG, "<Name> adapter shut down");
}

static void <name>_adapter_on_interval(supervisor_interval_stage_t stage) {
    // Handle periodic tasks
    if (stage == SUPERVISOR_INTERVAL_1S) {
        // Do something every second
    }
}
```

#### 5. Telemetry Functions (tele.h)
```c
static void tele_<name>_<data>(const char *tele_id, cJSON *json_root) {
    cJSON *obj = cJSON_CreateObject();
    // Add data to obj
    cJSON_AddItemToObject(json_root, tele_id, obj);
}
```

#### 6. Command Functions (cmnd.h)
```c
static void cmnd_<name>_<action>(const char *cmnd_id, const char *payload) {
    // Parse payload, execute action
    ESP_LOGI(TAG, "Command %s: %s", cmnd_id, payload);
}
```

### Naming Conventions
- TAG: `"cikon:adapter:<name>"` - used for ESP_LOGx macros (ESP_LOGI, ESP_LOGW, ESP_LOGE, ESP_LOGD)
  - Must be defined as `#define TAG "cikon:adapter:<name>"` at top of .c file
  - Use consistently in all logging: `ESP_LOGI(TAG, "message")`, `ESP_LOGE(TAG, "error: %s", err)`
- Kconfig prefix: `CONFIG_<NAME>_*` (uppercase)
- Functions: `<name>_adapter_*` or `tele_<name>_*` or `cmnd_<name>_*`
- File names: lowercase with underscores
- Header guards: **ALWAYS** use `#pragma once` (never `#ifndef` guards)

### CMakeLists.txt Template
```cmake
idf_component_register(
    SRCS "<name>.c"
    INCLUDE_DIRS "include"
    REQUIRES cikon_supervisor cikon_helpers cikon_mqtt
)
```

### Kconfig Template
```kconfig
menu "<Name> Adapter Configuration"
    config <NAME>_OPTION
        int "Option description"
        default 1
        range 1 100
endmenu
```
### idf_component.yml Template
**CRITICAL**: Always add external dependencies in `idf_component.yml`, NOT in CMakeLists.txt
```yaml
dependencies:
  espressif/component_name: "^1.0.0"
  espressif/another_component: "~2.0.0"
```

### Header Template
```c
#pragma once

#include "supervisor.h"

extern supervisor_platform_adapter_t <name>_adapter;
```spressif/component_name: "^1.0.0"
```

### Registration in main.c
```c
#include "<name>_adapter.h"

void app_main(void) {
    // ...
    supervisor_register_platform_adapter(&<name>_adapter);
    // ...
}
```

## Supervisor Intervals (supervisor.h)
Available stages for `on_interval`:
- `SUPERVISOR_INTERVAL_1S` - Every 1 second
- `SUPERVISOR_INTERVAL_5S` - Every 5 seconds  
- `SUPERVISOR_INTERVAL_10S` - Every 10 seconds
- `SUPERVISOR_INTERVAL_30S` - Every 30 seconds
- `SUPERVISOR_INTERVAL_1M` - Every 1 minute

## Home Assistant Integration (ha.h)

### Entity Types
```c
typedef enum {
    HA_SWITCH,
    HA_BINARY_SENSOR,
    HA_SENSOR,
    HA_LIGHT,
    HA_BUTTON
} ha_entity_type_t;
```

### Registration
```c
ha_register_entity(
    ha_entity_type_t type,
    const char *entity_id,      // Unique ID
    const char *device_class,   // HA device class or NULL
    const char *state_topic,    // MQTT state topic or NULL
    const char *command_topic    // MQTT command topic or NULL
);
```

## State Persistence (config_manager.h)

### Available Functions
```c
// String (max 63 chars)
esp_err_t config_manager_get_str(const char *key, char *out_value, size_t max_len);
esp_err_t config_manager_set_str(const char *key, const char *value);

// U8
esp_err_t config_manager_get_u8(const char *key, uint8_t *out_value);
esp_err_t config_manager_set_u8(const char *key, uint8_t value);

// U32
esp_err_t config_manager_get_u32(const char *key, uint32_t *out_value);
esp_err_t config_manager_set_u32(const char *key, uint32_t value);

// U64
esp_err_t config_manager_get_u64(const char *key, uint64_t *out_value);
esp_err_t config_manager_set_u64(const char *key, uint64_t value);
```

### Best Practices
- Use debouncing for frequently changing values
- Restore state in `init()` before HA registration
- Save state in response to commands or significant changes

## Example: Complete Adapter

See existing adapters for reference:
- **Simple**: `cikon_supervisor_adapters_button/` - GPIO input with debouncing
- **Output**: `cikon_supervisor_adapters_led/` - GPIO output with state persistence
- **Sensor**: `cikon_supervisor_adapters_ds18b20/` - I2C/1-Wire sensor reading
- **Complex**: `cikon_supervisor_adapters_debug/` - Multiple commands and telemetry

## Common Patterns

### Code Style
- **Minimize nesting depth** - use early returns (guard clauses) instead of deep if-else chains
  ```c
  // ❌ BAD - deep nesting
  void process(void) {
      if (initialized) {
          if (data != NULL) {
              if (validate(data)) {
                  // actual work here
              }
          }
      }
  }
  
  // ✅ GOOD - guard clauses
  void process(void) {
      if (!initialized) return;
      if (data == NULL) return;
      if (!validate(data)) return;
      
      // actual work here - no nesting
  }
  ```

### Error Handling
- Use `ESP_ERROR_CHECK()` for critical failures during init
- Use `ESP_LOGE/W/I/D` for logging
- Return gracefully from callbacks on non-critical errors
- Check `initialized` flag in shutdown/interval/event handlers

### Initialization Logging (MANDATORY)
**Every adapter init function MUST start with:**
```c
ESP_LOGI(TAG, "Initializing <adapter_name> adapter");
```
This provides clear visibility during boot sequence. Supervisor does NOT log adapter registration - each adapter identifies itself.

### Resource Management
- Always cleanup in `shutdown()`
- Use static globals for adapter state
- Initialize all struct members
- Set handles to NULL after freeing

### FreeRTOS Safety
- **Add `vTaskDelay(1)` in tight loops** to prevent task watchdog timeout
  - Required in loops with intensive operations (flash writes, crypto, sensor reads)
  - Yields control to IDLE task which resets watchdog
  - Example: After `esp_ota_write()`, `ds18b20_get_temperature()`, heavy processing
  - **CRITICAL for high-priority tasks** - they can monopolize CPU and starve IDLE
- Use proper synchronization for shared state
- Keep callback handlers short and non-blocking
- **Rule of thumb:** If loop runs >100ms without blocking I/O, add `vTaskDelay(1)`

## Platform Integration Metadata (HA, Zigbee, Matter)

Adapters can declare **platform-specific metadata** using a **zero-coupling pattern**. This works for any integration: Home Assistant (HA), Zigbee, Matter, Thread, etc. Adapters define static metadata structures, platform adapters iterate and register them.

**Key concept:** Adapters describe "what they are" (sensors, switches, endpoints), platform adapters handle "how to register" (MQTT discovery, Zigbee coordinator, Matter controller).

### Metadata Pattern Rules

#### 1. Include metadata.h (Always)
```c
#include "metadata.h"  // Always included - types for ALL platforms
```
- No `#ifdef` needed for the include
- Contains type definitions for **all** platforms (HA, Zigbee, Matter, etc.)
- Zero binary overhead when unused

#### 2. Define Custom Builders (If Needed)
```c
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
static void build_custom_ha_entity(cJSON *payload, const char *sanitized_name) {
    // Platform-specific builder logic (example: HA JSON)
}
#endif
```
- Custom builders go inside appropriate `#ifdef` for the target platform
- Only compiled when that platform integration is enabled

#### 3. Define Metadata Structure (Platform-Specific)

**Example: Home Assistant**
```c
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
static const ha_metadata_t my_adapter_ha_metadata = {
    .magic = HA_METADATA_MAGIC,
    .entities = {
        {.type = HA_SENSOR, .name = "Temperature", .device_class = "temperature"},
        {.type = HA_SWITCH, .name = "Power", .icon = "mdi:power"},
        {.type = HA_SENSOR, 
         .name = "Custom Entity",
         .entity_category = "diagnostic",
         .custom_builder = build_custom_ha_entity},
        {.type = HA_ENTITY_NONE}  // Sentinel - MANDATORY
    }};
#endif
```

**Future Example: Zigbee**
```c
#ifdef CONFIG_ZIGBEE_ENABLE
static const zigbee_metadata_t my_adapter_zigbee_metadata = {
    .magic = ZIGBEE_METADATA_MAGIC,
    .endpoints = {
        {.cluster = ZB_ON_OFF_CLUSTER, .endpoint_id = 1},
        {.cluster = ZB_LEVEL_CONTROL_CLUSTER, .endpoint_id = 1},
        {.cluster = ZB_CLUSTER_NONE}  // Sentinel
    }};
#endif
```

**Metadata Structure Rules:**
- Wrap entire structure in `#ifdef CONFIG_<PLATFORM>_*`
- Use correct magic constant for platform: `HA_METADATA_MAGIC`, `ZIGBEE_METADATA_MAGIC`, `MATTER_METADATA_MAGIC`
- **ALWAYS** end arrays with platform-specific sentinel
- Sentinel pattern: `{.type = <PLATFORM>_ENTITY_NONE}` or equivalent

#### 4. Add Metadata to Adapter Struct

**Single Platform:**
```c
supervisor_platform_adapter_t my_adapter = {
    .init = my_adapter_init,
    .shutdown = my_adapter_shutdown,
    .tele_group = my_telemetry,
    .cmnd_group = my_commands,
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    .metadata = &my_adapter_ha_metadata,
#endif
};
```

**Multi-Platform (Future):**
```c
#ifdef CONFIG_MQTT_ENABLE_HA_DISCOVERY
    .metadata = &my_adapter_ha_metadata,
#elif defined(CONFIG_ZIGBEE_ENABLE)
    .metadata = &my_adapter_zigbee_metadata,
#endif
```

- When disabled, field is zero-initialized (NULL) - no memory waste
- No `#else` branch needed for single platform (implicit zero-init)

#### 5. Dependencies (CRITICAL)
```yaml
# idf_component.yml
dependencies:
  cikon_helpers:  # For metadata.h types (ALL platforms)
    git: "https://github.com/pwilga/cikon-iot-solution.git"
    path: components/cikon_helpers
    require: public  # Only if adapter has public headers using metadata types
```

**Never add:**
- ❌ `cikon_mqtt` to adapter dependencies (HA-specific)
- ❌ `cikon_wifi` to adapter dependencies
- ❌ `cikon_zigbee` to adapter dependencies (Zigbee-specific)
- ❌ Any platform-specific network components

Adapters stay **hardware-focused**. Platform adapters (inet, zigbee, matter) handle registration.

#### 6. Platform Adapter Registration

**Example: inet adapter (HA)**
```c
// inet.c - SUPERVISOR_EVENT_PLATFORM_INITIALIZED handler
if (bits & SUPERVISOR_EVENT_PLATFORM_INITIALIZED) {
    inet_adapter_register_ha_entities();  // Iterates all adapter metadata
}

static void inet_adapter_register_ha_entities(void) {
    const supervisor_platform_adapter_t **adapters = supervisor_get_adapters();
    for (int i = 0; adapters[i] != NULL; i++) {
        if (adapters[i]->metadata == NULL) continue;
        
        const ha_metadata_t *meta = (const ha_metadata_t *)adapters[i]->metadata;
        if (meta->magic != HA_METADATA_MAGIC) continue;  // Verify magic
        
        for (int e = 0; meta->entities[e].type != HA_ENTITY_NONE; e++) {
            ha_register_entity(&meta->entities[e]);
        }
    }
}
```

**Future Example: Zigbee adapter**
```c
// zigbee.c - on initialization
static void zigbee_adapter_register_endpoints(void) {
    const supervisor_platform_adapter_t **adapters = supervisor_get_adapters();
    for (int i = 0; adapters[i] != NULL; i++) {
        if (adapters[i]->metadata == NULL) continue;
        
        const zigbee_metadata_t *meta = (const zigbee_metadata_t *)adapters[i]->metadata;
        if (meta->magic != ZIGBEE_METADATA_MAGIC) continue;
        
        for (int e = 0; meta->endpoints[e].cluster != ZB_CLUSTER_NONE; e++) {
            zigbee_register_endpoint(&meta->endpoints[e]);
        }
    }
}
```

#### 7. Sentinel Pattern
- **Entity arrays:** Use `{.type = <PLATFORM>_ENTITY_NONE}` as last element
- **Loop termination:** `meta->entities[e].type != <PLATFORM>_ENTITY_NONE`
- **Never** use raw `{0}` - first enum value is typically 1, sentinel is 0

#### 8. Magic Signatures
```c
#define HA_METADATA_MAGIC      0x48414D44  // "HAMD" - Home Assistant
#define ZIGBEE_METADATA_MAGIC  0x5A494742  // "ZIGB" - Zigbee
#define MATTER_METADATA_MAGIC  0x4D545452  // "MTTR" - Matter
```
- Allows safe runtime type identification
- Platform adapters check magic before casting
- Prevents accidental misinterpretation of metadata structures
- Future-proof for multi-protocol adapters

### Metadata Benefits
- ✅ Zero coupling - adapters don't depend on platform-specific components
- ✅ Zero overhead when disabled - `#ifdef` guards all structures
- ✅ Platform agnostic - same pattern for HA, Zigbee, Matter, Thread, ESP-NOW, etc.
- ✅ Works without network - ESP-NOW projects don't need inet adapter
- ✅ Multi-platform ready - adapters can have multiple metadata structures
- ✅ Type-safe - magic signatures prevent wrong casts
- ✅ Scalable - add new platforms without modifying existing adapters

### Common Patterns
- Use consistent naming (TAG, functions, Kconfig)
- Add proper error handling and logging
- Test init/shutdown cycle
- Document adapter-specific configuration in Kconfig
- Use `#pragma once` in ALL header files
- Declare external dependencies in `idf_component.yml` (espressif/* components)
- Only use CMakeLists.txt REQUIRES for internal cikon_* components

## DO NOT
- ❌ Create adapters outside the standard structure
- ❌ Skip NULL terminators in tele_group/cmnd_group
- ❌ Use blocking operations in callbacks
- ❌ Assume resources are initialized without checking
- ❌ Use `#ifndef` header guards (use `#pragma once` instead)
- ❌ Add external dependencies in CMakeLists.txt (use `idf_component.yml`)
- ❌ Use deep nesting - prefer early returns/guard clauses
- ❌ **GUESS configuration values - ALWAYS check examples first!**
- ❌ **Implement features without reading component examples**
- ❌ **Leave trailing whitespace in files (git will complain)**

## ALWAYS
- ✅ **Check `managed_components/<component>/examples/` or component docs FIRST**
- ✅ **Use EXACT values from official examples before experimenting**
- ✅ Follow existing adapter patterns exactly
- ✅ Use consistent naming (TAG, functions, Kconfig)
- ✅ Add proper error handling and logging
- ✅ Test init/shutdown cycle
- ✅ Document adapter-specific configuration in Kconfig
- ✅ Use `#pragma once` in ALL header files
- ✅ Declare external dependencies in `idf_component.yml` (espressif/* components)
- ✅ Only use CMakeLists.txt REQUIRES for internal cikon_* components
- ✅ **Remove trailing whitespace before committing (keep code clean)**
