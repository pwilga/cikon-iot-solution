# TCP Monitor

A lightweight TCP-based log monitoring component for ESP32 that streams ESP_LOG output over the network with ANSI color support.

## Features

- **Thread-safe**: Uses FreeRTOS ring buffer (4KB) to separate logging from TCP send
- **Color-coded logs**: Automatic ANSI coloring for ERROR (red), WARNING (yellow), INFO (green), default (white)
- **Dual output**: Maintains UART logging while streaming to TCP client
- **Single active client**: Accepts one TCP connection at a time (others wait in queue)
- **Memory-safe**: Fixed-size ring buffer (NOSPLIT) prevents memory growth
- **Safe shutdown**: Properly closes connections and restores UART logging
- **Configurable port**: Change port via `tcp_monitor_configure()` before init

## Usage

### Configuration

```c
#include "tcp_monitor.h"

// Use default port (6666)
tcp_monitor_init();

// Or configure custom port
tcp_monitor_configure(5005);
tcp_monitor_init();

// Shutdown
tcp_monitor_shutdown();
```

### Connecting with netcat (nc)

```bash
# Connect to ESP32
nc <ESP32_IP> 6666

# Exit: Ctrl+C
```

### Connecting with telnet

```bash
# Connect to ESP32
telnet <ESP32_IP> 6666

# Exit: Ctrl+] then type 'quit'
```

### Connecting with socat (recommended for colors)

```bash
# Install socat (macOS)
brew install socat

# Connect to ESP32
socat - TCP:<ESP32_IP>:6666

# Exit: Ctrl+C
```

## How It Works

1. **Server Setup**: Creates TCP server socket listening on configured port (default 6666)
2. **Client Connection**: Accepts single client connection, additional clients wait in TCP queue
3. **Log Redirection**: Registers custom `vprintf` handler via `esp_log_set_vprintf()`
4. **Producer**: Every `ESP_LOG*()` formats log (max 128B) and sends to ring buffer (non-blocking)
5. **Consumer**: Task drains ring buffer every 100ms, adds ANSI colors, sends to TCP client
6. **Color Detection**: Checks first 3 chars ("I (", "W (", "E (") for automatic coloring
7. **Cleanup**: Restores UART-only logging and deletes ring buffer on shutdown

## Limitations

- **Single client**: Only one TCP connection at a time (new connections rejected while client connected)
- **No buffering**: Messages may be dropped if client is slow (by design)
- **Port change**: Requires shutdown/restart to change port
- **Color detection**: Simple first-character check (E/W/I)

## Behavior with Multiple Clients

When a client is connected:
- New connection attempts **wait** in TCP backlog (queue size = 1)
- Second client blocks on `connect()` until first client disconnects
- After disconnect, waiting client is immediately accepted
- No crashes or rejections - just queuing
