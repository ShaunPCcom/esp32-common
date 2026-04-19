// SPDX-License-Identifier: MIT
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @file web_server_base.h
 * @brief Shared HTTP server infrastructure for ESP32-C6 web UI.
 *
 * Provides: SPIFFS asset serving, WiFi provisioning endpoints, OTA endpoints,
 * system endpoints, and a captive portal for AP mode setup.
 *
 * Device-specific endpoints are registered via web_server_base_register()
 * after calling web_server_base_start().
 */

typedef struct {
    /** Short display name, e.g. "LD2450" or "LED Controller". Used in the
     *  AP-mode setup page title and heading. */
    const char *device_name;

    /** Firmware version string with 'v' prefix, e.g. "v2.3.2". Used in the
     *  setup page info section and GET /api/status response. */
    const char *firmware_version;

    /** NVS namespace shared with the device project, e.g. "ld2450_cfg".
     *  Used to persist the web asset version key and OTA settings. */
    const char *nvs_namespace;

    /** OTA image type for this device variant, forwarded to ota_check_init(). */
    uint16_t ota_image_type;

    /** Running firmware version as hex, e.g. FIRMWARE_VERSION. Forwarded to
     *  ota_check_init() so it can compare against the latest index entry. */
    uint32_t current_version_hex;

    /* Web asset binary data — from target_add_binary_data() in CMakeLists.txt.
     * Size = end - start (TEXT mode adds a null byte that is stripped). */
    const uint8_t *index_html_start;
    size_t         index_html_size;
    const uint8_t *app_js_start;
    size_t         app_js_size;
    const uint8_t *style_css_start;
    size_t         style_css_size;
} web_server_base_config_t;

/**
 * Start the shared HTTP server.
 *
 * Mounts SPIFFS, syncs web assets, registers all common endpoints (WiFi, OTA,
 * system, captive portal), and calls ota_check_init().
 *
 * @param cfg  Device config. Must remain valid for the lifetime of the server.
 * @return ESP_OK on success, ESP_FAIL if httpd_start() fails.
 */
esp_err_t web_server_base_start(const web_server_base_config_t *cfg);

/**
 * Register a device-specific URI handler on the running server.
 *
 * Must be called after web_server_base_start(). The @p uri pointer must remain
 * valid for the lifetime of the server (use a string literal or static buffer).
 *
 * @param uri          URI path, e.g. "/api/config"
 * @param method       HTTP method, e.g. HTTP_GET or HTTP_POST
 * @param handler      Handler function
 * @param is_websocket True for WebSocket upgrade endpoints
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if server not started.
 */
esp_err_t web_server_base_register(const char *uri, httpd_method_t method,
                                   esp_err_t (*handler)(httpd_req_t *req),
                                   bool is_websocket);

/** Stop the HTTP server and free the setup page buffer. */
void web_server_base_stop(void);

/* ========================================================================== */
/*  Server-Sent Events (SSE)                                                   */
/* ========================================================================== */

/**
 * Serializer called once per topic per connected client when a topic's version
 * changes. Write a JSON document into @p buf; return bytes written (excluding
 * trailing NUL), or < 0 on error. Runs in the SSE client task context.
 */
typedef int (*wsb_sse_serializer_t)(const char *topic, char *buf, size_t buf_len);

/**
 * Register a streaming endpoint at @p uri. @p topics is a NULL-terminated
 * array of topic name string literals (e.g. {"segments","config",NULL}).
 * The serializer is called whenever a topic's version changes. Must be called
 * after web_server_base_start(). Only one endpoint may be registered.
 */
esp_err_t web_server_base_sse_register(const char *uri,
                                       const char *const *topics,
                                       wsb_sse_serializer_t serializer);

/**
 * Bump a topic's version counter and wake all connected SSE clients.
 * Safe from any task context (not ISR-safe). O(1).
 */
void web_server_base_sse_notify(const char *topic);
