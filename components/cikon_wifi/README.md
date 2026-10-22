# Cikon WiFi Component

WiFi station (STA) with an on-demand access point (AP) next to it, for Cikon ESP-IDF projects.
The component knows nothing about the app config: credentials are passed in by the caller.

## API

| Function | Purpose |
|---|---|
| `wifi_start(creds)` | Init the driver, create interfaces, start STA and connect to `creds->sta_ssid`. Stores the AP SSID for later. |
| `wifi_sta_connect(ssid, pass)` | Switch STA to another network at runtime, no restart. The first attempt starts right away. |
| `wifi_ap_enable()` / `wifi_ap_disable()` | Add / remove the AP next to STA (`WIFI_MODE_APSTA` ↔ `WIFI_MODE_STA`). |
| `wifi_shutdown()` | Stop WiFi, destroy interfaces, deinit the driver. |
| `wifi_get_interface_ip()`, `wifi_get_rssi()`, `wifi_get_ssid()` | Status for telemetry. RSSI and SSID only while STA has an IP. |

Strings passed to `wifi_start()` / `wifi_sta_connect()` must stay valid while WiFi runs - only the
pointers are kept.

## STA connection

- Attempts run in bursts: `WIFI_STA_BURST_RETRY_COUNT` attempts, `WIFI_STA_BURST_RETRY_DELAY_MS`
  apart. After a failed burst comes a pause starting at `WIFI_STA_RETRY_INITIAL_MS`, doubling up to
  `WIFI_STA_RETRY_MAX_MS`. Getting an IP resets the counter and the pause.
- The minimum accepted security is WPA2-PSK - open and WEP networks are not joined.

## Access point

Open AP (`WIFI_AUTH_OPEN`, 1 client), SSID from `creds->ap_ssid`. While it runs, services listening
on any address (HTTP, OTA, ...) answer on `192.168.4.1` as well. In APSTA the AP follows the STA
channel, so a STA connecting to another channel briefly drops AP clients.

### When the AP opens

- **By hand:** `wifi_ap_enable()` (command, button).
- **Automatically, once per boot:** when the first burst after boot fails and STA has never had an IP.
  Later failures (e.g. the router off for a day) don't open it again.

### When the AP closes

| Situation | AP |
|---|---|
| STA has no IP, AP opened by hand | stays on |
| STA has no IP, AP opened automatically, nobody connected | closes after `WIFI_AP_INACTIVITY_TIMEOUT_MINUTES` |
| a client connects to an automatically opened AP | treated as opened by hand from then on |
| AP starts (or is enabled again) while STA has an IP | closes after `WIFI_AP_INACTIVITY_TIMEOUT_MINUTES` |
| STA gets an IP, no client on the AP | closes right away |
| STA gets an IP, a client is connected | closes after 20 s, or earlier on `wifi_ap_disable()` |
| STA loses its IP | stays on (except an unused automatically opened AP, which keeps its limit) |

Connected clients never extend the time left.

## Configuration (Kconfig)

| Option | Default | Meaning |
|---|---|---|
| `WIFI_AP_INACTIVITY_TIMEOUT_MINUTES` | 1 | AP lifetime, see the table above |
| `WIFI_STA_BURST_RETRY_COUNT` | 5 | attempts per burst |
| `WIFI_STA_BURST_RETRY_DELAY_MS` | 10000 | delay between attempts in a burst |
| `WIFI_STA_RETRY_INITIAL_MS` | 60000 | first pause after a failed burst |
| `WIFI_STA_RETRY_MAX_MS` | 600000 | pause upper limit |
