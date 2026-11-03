// SPDX-License-Identifier: MIT
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * @file wifi_manager.h
 * @brief WiFi AP/STA manager for ESP32-C6 web configuration interface.
 *
 * State machine:
 *   INIT → read NVS credentials
 *     → no creds:    AP mode (<prefix>-XXXX, open)  + captive DNS
 *     → has creds:   STA connecting
 *       → connected:    STA_CONNECTED
 *       → disconnected: retry forever (fast, then 30s backoff) — never AP
 *
 * Invariant: AP mode and Zigbee never run in the same boot (softAP/802.15.4
 * coex is unstable). AP is only entered at boot with no credentials.
 *
 * Compiled only on ESP32-C6 via CONFIG_IDF_TARGET_ESP32C6 guard in CMakeLists.
 */

typedef enum {
    WIFI_MGR_STATE_INIT = 0,
    WIFI_MGR_STATE_AP,
    WIFI_MGR_STATE_STA_CONNECTING,
    WIFI_MGR_STATE_STA_CONNECTED,
    WIFI_MGR_STATE_STA_FAILED,      /* unused — kept for API/UI compat */
} wifi_mgr_state_t;

/**
 * Initialize WiFi subsystem (netif, event loop, WiFi driver).
 * @param hostname_prefix  Short prefix used for AP SSID and fallback hostname,
 *                         e.g. "ld2450" → "ld2450-A1B2C3". Max 15 chars.
 * Call once at boot before wifi_manager_start().
 */
void wifi_manager_init(const char *hostname_prefix);

/** Start the WiFi manager — reads NVS and enters AP or STA mode. */
void wifi_manager_start(void);

/**
 * Lock out AP mode. Call before starting Zigbee; any later attempt to start
 * the AP is refused and logged.
 */
void wifi_manager_set_ap_locked(bool locked);

/** Return current manager state. */
wifi_mgr_state_t wifi_manager_get_state(void);

/** True when in STA_CONNECTED state. */
bool wifi_manager_is_connected(void);

/** True when in AP mode (provisioning — no credentials). */
bool wifi_manager_is_ap_mode(void);

/**
 * Persist WiFi credentials to NVS.  Does NOT trigger reconnect — caller
 * should restart or call wifi_manager_start() again.
 */
esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password);

/** Erase stored WiFi credentials from NVS. */
esp_err_t wifi_manager_clear_credentials(void);

/** True if WiFi credentials (SSID) are stored in NVS. */
bool wifi_manager_has_credentials(void);

/** Persist device hostname to NVS wifi_cfg namespace. */
esp_err_t wifi_manager_save_hostname(const char *hostname);

/**
 * Read stored hostname into buf.  Returns true if a hostname was found,
 * false if the key is missing or buf is too small.
 */
bool wifi_manager_get_hostname(char *buf, size_t len);
