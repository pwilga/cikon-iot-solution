# TCP OTA Component

The TCP OTA component provides Over-The-Air (OTA) firmware update functionality over TCP for ESP-IDF projects with a secure binary protocol.

## Features
- OTA updates via TCP connection with binary protocol
- Magic byte handshake for connection validation
- Software version exchange
- MD5 checksum verification
- Automatic partition switching after successful update
- Error handling and recovery
- Integration with ESP-IDF OTA subsystem

## Protocol Overview

The TCP OTA uses a structured binary protocol with acknowledgment steps:

### Protocol Sequence

1. **Magic Bytes Handshake**
   - Client sends: `0xAF 0xCA 0xEC 0x2D 0xFE 0x55` (6 bytes)
   - Server responds: `0xAA 0x55` (ACK - 2 bytes)

2. **Software Version Exchange**
   - Client sends: 2 bytes (e.g., `0x09 0x05` for v9.5)
   - Server responds: `0xAA 0x55` (ACK)

3. **Firmware Size**
   - Client sends: 4 bytes (big-endian uint32)
   - Server responds: `0xAA 0x55` (ACK)

4. **MD5 Checksum**
   - Client sends: 16 bytes (MD5 hash of firmware)
   - Server responds: `0xAA 0x55` (ACK)

5. **Firmware Binary Transfer**
   - Client sends: Complete firmware binary
   - Client closes write end of socket (`SHUT_WR`)
   - Server validates MD5 and responds: `0xAA 0x55` (ACK)

6. **Completion**
   - Server validates the firmware
   - Marks partition as bootable
   - Reboots to apply update

### Protocol Constants

| Constant | Value | Description |
|----------|-------|-------------|
| Magic Bytes | `0xAF 0xCA 0xEC 0x2D 0xFE 0x55` | Connection handshake |
| ACK | `0xAA 0x55` | Acknowledgment response |
| Port | `5555` | Default TCP port |

## Quick Start

### Server Side (ESP32)

#### Initialization
```c
#include "tcp_ota.h"

void app_main(void) {
    // Initialize TCP OTA server on port 5555
    tcp_ota_init();
}
```

### Client Side (Python)

#### Sending OTA Update
```python
import socket
import hashlib
from pathlib import Path

ESP_IP = "192.168.1.100"  # ESP32 IP address
ESP_PORT = 5555

# Protocol constants
magic_bytes = bytes([0xAF, 0xCA, 0xEC, 0x2D, 0xFE, 0x55])
expected_ack = bytes([0xAA, 0x55])
sw_version = bytes([0x09, 0x05])  # Version 9.5

def check_ack(s):
    """Wait for and verify 2-byte ACK from ESP32"""
    ack = s.recv(2)
    if ack == expected_ack:
        print("✅ Received ACK from ESP32")
    else:
        print(f"❌ Unexpected response: {ack.hex()}")
        sys.exit(1)

# Load firmware
firmware_file = Path("build/firmware.bin")
firmware_size = firmware_file.stat().st_size
fw_size_bytes = firmware_size.to_bytes(4, byteorder="big")

print(f"Firmware size: {firmware_size} bytes")

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.connect((ESP_IP, ESP_PORT))
    
    # Step 1: Send magic bytes
    s.sendall(magic_bytes)
    check_ack(s)
    
    # Step 2: Send software version
    s.sendall(sw_version)
    check_ack(s)
    
    # Step 3: Send firmware size
    s.sendall(fw_size_bytes)
    check_ack(s)
    
    # Step 4: Calculate and send MD5 hash
    with open(firmware_file, "rb") as f:
        firmware_data = f.read()
        md5_hash = hashlib.md5(firmware_data).digest()
        print(f"MD5: {md5_hash.hex()}")
        
        s.sendall(md5_hash)
        check_ack(s)
        
        # Step 5: Send firmware binary
        s.sendall(firmware_data)
        s.shutdown(socket.SHUT_WR)
        check_ack(s)
        
    print("✅ OTA update complete!")
```

## Error Handling

The component validates each step and handles errors:
- Invalid magic bytes → connection rejected
- Insufficient partition space → update aborted
- MD5 mismatch → update rejected
- Write failures → automatic rollback

All errors are logged via ESP-IDF logging system with tag `TCP_OTA`.

## Configuration

**Default Port:** `5555`

To change the port, modify `TCP_OTA_PORT` in the source code.

## API Documentation

### Functions

#### `void tcp_ota_init(void)`
Initializes the TCP OTA server and starts listening for connections on port 5555.

**Parameters:** None

**Returns:** None

**Note:** This function creates a FreeRTOS task that runs indefinitely and handles incoming OTA requests.

## Dependencies
- ESP-IDF OTA subsystem (`app_update`)
- TCP/IP stack (`lwip`)
- mbedTLS (for MD5 verification)
- cikon_core

See `CMakeLists.txt` for complete dependency list.

## Security Features
- Magic byte handshake prevents accidental connections
- MD5 checksum verification ensures firmware integrity
- Binary protocol reduces parsing vulnerabilities

## Security Considerations

⚠️ **Important:** This implementation is suitable for development and trusted networks only.

For production use, consider adding:
- **TLS/SSL encryption** for data in transit
- **Digital signature verification** (RSA/ECDSA)
- **Authentication tokens** or password protection
- **Firmware encryption** at rest
- **Rollback protection** mechanisms
- **Rate limiting** to prevent DoS attacks

## Notes
- The device will automatically reboot after a successful OTA update
- Ensure sufficient heap memory for OTA operations (minimum ~128KB recommended)
- Only one OTA session is supported at a time
- The component validates MD5 checksum before applying the update
- Connection timeout: configurable via ESP-IDF settings

## Troubleshooting

### Common Issues

**Connection Refused**
- Check ESP32 IP address and port
- Verify WiFi connection
- Check firewall settings

**MD5 Mismatch**
- Ensure firmware file is not corrupted
- Verify file is not being modified during transfer
- Check network stability

**Insufficient Space**
- Verify OTA partition size in partition table
- Ensure firmware size < OTA partition size

## Example Use Cases
- Development and testing environments
- Local network firmware updates
- Factory provisioning and testing
- Automated CI/CD deployment pipelines
- Field updates in controlled networks