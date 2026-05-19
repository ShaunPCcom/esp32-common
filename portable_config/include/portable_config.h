// SPDX-License-Identifier: MIT
#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include "esp_zigbee_core.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @file portable_config.h
 * @brief Portable JSON config import/export for ESP32 Zigbee projects.
 *
 * Provides a versioned JSON bundle of all user-configurable settings that can
 * be transferred between chip variants (C6 ↔ H2) of the same project.
 *
 * Transports:
 *   - C6: HTTP GET/POST /api/backup (file download/upload in web UI)
 *   - All chips: Zigbee custom cluster 0xFC10 (chunked attribute transfer)
 */

/* ---- Zigbee attribute IDs (added to the project's existing custom cluster) ---- */
/* Use 0x0100–0x0105 to avoid collisions with LD2450/LED attrs below 0x00FF.    */
#define PC_ATTR_CHUNK_IDX       0x0100  /* U16  RW  chunk index for export read / import write  */
#define PC_ATTR_CHUNK_DATA      0x0101  /* CHAR_STRING RW  64-byte chunk payload               */
#define PC_ATTR_CHUNK_COUNT     0x0102  /* U16  R+Rep  total chunks in current export buffer    */
#define PC_ATTR_TRIGGER         0x0103  /* U8   W  1=start_export 2=commit_import 3=abort       */
#define PC_ATTR_STATUS          0x0104  /* U8   R+Rep  0=idle 1=exporting 2=importing 3=ok 4=err */
#define PC_ATTR_ERROR_CODE      0x0105  /* U8   R+Rep  see PC_ERR_* constants                  */

/* Error codes reported in PC_ATTR_ERROR_CODE */
#define PC_ERR_NONE             0
#define PC_ERR_SCHEMA           1  /* Unsupported envelope schema version */
#define PC_ERR_DEVICE           2  /* Device tag mismatch                 */
#define PC_ERR_SIZE             3  /* Bundle exceeds 8 KB limit           */
#define PC_ERR_PARSE            4  /* JSON parse failure                  */
#define PC_ERR_APPLY            5  /* Deserialize callback returned error */
#define PC_ERR_BUSY             6  /* Transfer already in progress        */

/* Status values reported in PC_ATTR_STATUS */
#define PC_STATUS_IDLE          0
#define PC_STATUS_EXPORTING     1
#define PC_STATUS_IMPORTING     2
#define PC_STATUS_OK            3
#define PC_STATUS_ERR           4

#define PC_CHUNK_SIZE           64   /* bytes per ZCL chunk                */
#define PC_MAX_BUNDLE_SIZE      8192 /* maximum serialized bundle bytes    */
#define PC_WATCHDOG_SEC         30   /* stale-transfer timeout             */

/** Produces the device-specific payload cJSON object (caller owns it). */
typedef esp_err_t (*pc_serialize_cb_t)(cJSON **out);

/** Applies a device-specific payload cJSON object. Called from Zigbee task. */
typedef esp_err_t (*pc_deserialize_cb_t)(const cJSON *payload);

typedef struct {
    const char          *device_tag;      /* e.g. "ld2450" or "led_ctrl" — validated on import */
    uint16_t             schema_version;  /* payload schema version; project-owned             */
    const char          *fw_version;      /* from version.h, informational                     */
    uint16_t             cluster_id;      /* ZCL cluster hosting the PC attrs (e.g. 0xFC00)   */
    pc_serialize_cb_t    serialize;
    pc_deserialize_cb_t  deserialize;
} portable_config_init_t;

/**
 * Initialise the component. Must be called once before any transport is registered.
 * Allocates the internal 8 KB transfer buffer.
 */
esp_err_t portable_config_init(const portable_config_init_t *cfg);

/**
 * Register HTTP /api/backup endpoints (GET = export, POST = import).
 * C6 only — call after web_server_base_start(). No-op on H2.
 */
esp_err_t portable_config_register_http(void);

/**
 * Add the portable-config attributes to an existing custom cluster attribute list.
 * Call this after adding all project-specific attrs to the list, before passing
 * the list to esp_zb_cluster_list_add_custom_cluster().
 * Must be called before esp_zb_device_register().
 */
esp_err_t portable_config_add_attrs(esp_zb_attribute_list_t *attr_list);

/**
 * Record which endpoint hosts the portable-config cluster.
 * Call once after esp_zb_device_register(), passing the EP number (e.g. ZB_EP_MAIN).
 */
void portable_config_set_endpoint(uint8_t ep);

/**
 * Dispatch an attribute write for cluster PC_CLUSTER_ID.
 * Call from the project's Zigbee attribute handler when cluster == PC_CLUSTER_ID.
 * @param attr_id  Attribute identifier from the ZCL message.
 * @param val      Raw attribute value pointer (ZCL format).
 */
esp_err_t portable_config_handle_attr_write(uint16_t attr_id, void *val);

/**
 * Write a JSON status document into buf (for SSE "backup" topic serializer).
 * Returns bytes written (excluding NUL), or < 0 on error.
 */
int portable_config_get_status_json(char *buf, size_t buf_len);
