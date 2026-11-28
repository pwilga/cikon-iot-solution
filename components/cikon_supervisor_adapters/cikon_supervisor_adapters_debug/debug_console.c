#include "debug_console.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "esp_console.h"
#include "esp_log.h"
#include "esp_stdio.h"
#include "linenoise/linenoise.h"

#include "cmnd.h"
#include "config_manager.h"
#include "json_parser.h"

#define TAG "cikon:debug:console"

#define CONSOLE_MAX_LINE 256
#define CONSOLE_HISTORY_LINES 32

static void debug_console_help(void) {
    size_t count = 0;
    const command_t *commands = cmnd_get_registry(&count);

    for (size_t i = 0; i < count; i++) {
        printf("  %-15s - %s\n", commands[i].command_id, commands[i].description);
    }
#if CONFIG_OPENTHREAD_CLI
    printf("  %-15s - %s\n", "ot <command>", "OpenThread CLI, \"ot help\" lists its commands");
#endif
}

// Tab completes the command names
static void debug_console_complete(const char *buf, linenoiseCompletions *completions) {
    size_t len = strlen(buf);
    size_t count = 0;
    const command_t *commands = cmnd_get_registry(&count);

    for (size_t i = 0; i < count; i++) {
        if (strncmp(commands[i].command_id, buf, len) == 0) {
            linenoiseAddCompletion(completions, commands[i].command_id);
        }
    }
#if CONFIG_OPENTHREAD_CLI
    if (strncmp("ot", buf, len) == 0) {
        linenoiseAddCompletion(completions, "ot");
    }
#endif
}

// Everything after the name is the argument of a supervisor command. JSON goes as typed, a plain
// word such as off becomes a JSON string, nothing becomes "null", as it does over the network
static void debug_console_submit(const char *name, const char *args) {
    if (args[0] == '\0') {
        cmnd_submit(name, "null");
        return;
    }

    cJSON *json = cJSON_ParseWithOpts(args, NULL, true);
    if (json) {
        cJSON_Delete(json);
        cmnd_submit(name, args);
        return;
    }

    if (args[0] == '{' || args[0] == '[') {
        printf("Invalid JSON\n");
        return;
    }

    json = cJSON_CreateString(args);
    char *quoted = cJSON_PrintUnformatted(json);
    if (quoted) {
        cmnd_submit(name, quoted);
        cJSON_free(quoted);
    } else {
        printf("No memory\n");
    }
    cJSON_Delete(json);
}

// A supervisor command goes to the supervisor, any other line to esp_console, where ot is
static void debug_console_run(char *line) {
    char *name = line + strspn(line, " ");
    size_t name_len = strcspn(name, " ");
    if (name_len == 0) {
        return;
    }

    char *args = name + name_len;
    args += strspn(args, " ");
    for (size_t len = strlen(args); len > 0 && args[len - 1] == ' '; len--) {
        args[len - 1] = '\0';
    }

    // The name is cut off for the lookup only: esp_console takes the line as it was
    char after_name = name[name_len];
    name[name_len] = '\0';

    if (strcmp(name, "help") == 0) {
        debug_console_help();
    } else if (cmnd_find(name)) {
        debug_console_submit(name, args);
    } else {
        name[name_len] = after_name;

        int ret = 0;
        esp_err_t err = esp_console_run(name, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("Unrecognized command, help lists them\n");
        } else if (err != ESP_OK) {
            printf("Console error: %s\n", esp_err_to_name(err));
        }
    }
}

static void debug_console_task(void *arg) {
    const char *prompt = arg;

    setvbuf(stdin, NULL, _IONBF, 0);

    // A terminal that does not answer gets the dumb mode, without arrows and history. Asking
    // blindly would eat the first characters typed
    linenoiseSetDumbMode(linenoiseProbe() != 0);

    while (true) {
        char *line = linenoise(prompt);

        // A terminal attached after the start answers once a line is entered
        if (linenoiseIsDumbMode() && linenoiseProbe() == 0) {
            linenoiseSetDumbMode(0);
        }

        if (line == NULL) {
            // An empty line, or no input at the moment
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (line[0] != '\0') {
            linenoiseHistoryAdd(line);
            debug_console_run(line);
        }
        linenoiseFree(line);
    }
}

// The supervisor may init the adapter again, the console starts only once
void debug_console_start(void) {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;

    static char prompt[20];
    snprintf(prompt, sizeof(prompt), "%s> ", config_get()->dev_name);

    fflush(stdout);
    fsync(fileno(stdout));
    ESP_ERROR_CHECK(esp_stdio_install_io_driver());

    esp_console_config_t console_config = ESP_CONSOLE_CONFIG_DEFAULT();
    console_config.max_cmdline_length = CONSOLE_MAX_LINE;
    ESP_ERROR_CHECK(esp_console_init(&console_config));

    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(&debug_console_complete);
    linenoiseSetMaxLineLen(CONSOLE_MAX_LINE);
    linenoiseHistorySetMaxLen(CONSOLE_HISTORY_LINES);

    if (xTaskCreate(debug_console_task, "console_repl", 4096, prompt, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Start: no memory for the task");
    }
}
