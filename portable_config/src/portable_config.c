// SPDX-License-Identifier: MIT
#include "portable_config.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "pc";

/* ---- Internal state ---- */

typedef struct {
    portable_config_init_t cfg;
    char    *buf;        /* PC_MAX_BUNDLE_SIZE bytes, malloc'd on init */
    size_t   buf_len;    /* bytes used in buf (valid after export or during import) */
    size_t   write_pos;  /* highest byte written during Zigbee import  */
    uint8_t  status;
    uint8_t  error_code;
    uint16_t chunk_count;
    int64_t  last_activity_us; /* for watchdog */
    uint8_t  endpoint;
    uint16_t cluster_id;
    bool     initialised;
} pc_state_t;

static pc_state_t s = {0};

/* ---- Helpers ---- */

static void set_status(uint8_t status, uint8_t err)
{
    s.status     = status;
    s.error_code = err;
    /* Update ZCL attribute values so Z2M can read them */
    if (s.initialised && s.endpoint) {
        esp_zb_zcl_set_attribute_val(s.endpoint, s.cluster_id,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, PC_ATTR_STATUS, &s.status, false);
        esp_zb_zcl_set_attribute_val(s.endpoint, s.cluster_id,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, PC_ATTR_ERROR_CODE, &s.error_code, false);
    }
}

static void reset_transfer(void)
{
    s.buf_len    = 0;
    s.write_pos  = 0;
    s.chunk_count = 0;
    set_status(PC_STATUS_IDLE, PC_ERR_NONE);
}

static bool watchdog_expired(void)
{
    if (!s.last_activity_us) return false;
    int64_t elapsed_sec = (esp_timer_get_time() - s.last_activity_us) / 1000000LL;
    return elapsed_sec > PC_WATCHDOG_SEC;
}

/* ---- Public API ---- */

esp_err_t portable_config_init(const portable_config_init_t *cfg)
{
    if (!cfg || !cfg->device_tag || !cfg->serialize || !cfg->deserialize) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s.initialised) return ESP_OK;

    s.buf = malloc(PC_MAX_BUNDLE_SIZE);
    if (!s.buf) {
        ESP_LOGE(TAG, "failed to alloc %d-byte bundle buffer", PC_MAX_BUNDLE_SIZE);
        return ESP_ERR_NO_MEM;
    }

    s.cfg        = *cfg;
    s.cluster_id = cfg->cluster_id;
    s.status      = PC_STATUS_IDLE;
    s.error_code  = PC_ERR_NONE;
    s.initialised = true;

    ESP_LOGI(TAG, "init: device=%s schema=%u", cfg->device_tag, cfg->schema_version);
    return ESP_OK;
}

/* ---- Export ---- */

esp_err_t portable_config_do_export(void)
{
    if (!s.initialised) return ESP_ERR_INVALID_STATE;

    if (s.status == PC_STATUS_EXPORTING || s.status == PC_STATUS_IMPORTING) {
        if (!watchdog_expired()) {
            set_status(PC_STATUS_ERR, PC_ERR_BUSY);
            return ESP_ERR_INVALID_STATE;
        }
        reset_transfer();
    }

    set_status(PC_STATUS_EXPORTING, PC_ERR_NONE);
    s.last_activity_us = esp_timer_get_time();

    cJSON *payload = NULL;
    esp_err_t err = s.cfg.serialize(&payload);
    if (err != ESP_OK || !payload) {
        ESP_LOGE(TAG, "serialize failed: %d", err);
        cJSON_Delete(payload);
        set_status(PC_STATUS_ERR, PC_ERR_APPLY);
        return err;
    }

    /* Wrap in versioned envelope */
    cJSON *root = cJSON_CreateObject();
    if (!root) { cJSON_Delete(payload); set_status(PC_STATUS_ERR, PC_ERR_APPLY); return ESP_ERR_NO_MEM; }
    cJSON_AddNumberToObject(root, "schema",         1);
    cJSON_AddStringToObject(root, "device",         s.cfg.device_tag);
    cJSON_AddNumberToObject(root, "device_schema",  s.cfg.schema_version);
    cJSON_AddStringToObject(root, "fw_version",     s.cfg.fw_version ? s.cfg.fw_version : "");
    cJSON_AddNumberToObject(root, "exported_at_unix", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddItemToObject(root, "payload", payload);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        set_status(PC_STATUS_ERR, PC_ERR_APPLY);
        return ESP_ERR_NO_MEM;
    }

    size_t len = strlen(json_str);
    if (len >= PC_MAX_BUNDLE_SIZE) {
        ESP_LOGE(TAG, "bundle too large: %u bytes", (unsigned)len);
        free(json_str);
        set_status(PC_STATUS_ERR, PC_ERR_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(s.buf, json_str, len + 1);
    s.buf_len     = len;
    s.chunk_count = (uint16_t)((len + PC_CHUNK_SIZE - 1) / PC_CHUNK_SIZE);
    free(json_str);

    /* Update chunk_count ZCL attribute */
    if (s.endpoint) {
        esp_zb_zcl_set_attribute_val(s.endpoint, s.cluster_id,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, PC_ATTR_CHUNK_COUNT, &s.chunk_count, false);
    }

    set_status(PC_STATUS_EXPORTING, PC_ERR_NONE);
    ESP_LOGI(TAG, "export ready: %u bytes, %u chunks", (unsigned)s.buf_len, s.chunk_count);
    return ESP_OK;
}

/* ---- Import ---- */

esp_err_t portable_config_do_import(const char *json_str, size_t len)
{
    if (!s.initialised) return ESP_ERR_INVALID_STATE;
    if (len == 0 || len >= PC_MAX_BUNDLE_SIZE) {
        set_status(PC_STATUS_ERR, PC_ERR_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    set_status(PC_STATUS_IMPORTING, PC_ERR_NONE);

    cJSON *root = cJSON_ParseWithLength(json_str, len);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        set_status(PC_STATUS_ERR, PC_ERR_PARSE);
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate envelope */
    cJSON *schema_j = cJSON_GetObjectItem(root, "schema");
    cJSON *device_j = cJSON_GetObjectItem(root, "device");
    cJSON *dev_sch  = cJSON_GetObjectItem(root, "device_schema");
    cJSON *payload  = cJSON_GetObjectItem(root, "payload");

    if (!cJSON_IsNumber(schema_j) || (int)schema_j->valuedouble != 1) {
        ESP_LOGE(TAG, "unsupported envelope schema: %d",
                 schema_j ? (int)schema_j->valuedouble : -1);
        cJSON_Delete(root);
        set_status(PC_STATUS_ERR, PC_ERR_SCHEMA);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!cJSON_IsString(device_j) || strcmp(device_j->valuestring, s.cfg.device_tag) != 0) {
        ESP_LOGE(TAG, "device mismatch: got '%s', want '%s'",
                 cJSON_IsString(device_j) ? device_j->valuestring : "?",
                 s.cfg.device_tag);
        cJSON_Delete(root);
        set_status(PC_STATUS_ERR, PC_ERR_DEVICE);
        return ESP_ERR_INVALID_ARG;
    }

    if (!cJSON_IsNumber(dev_sch) || (uint16_t)dev_sch->valuedouble > s.cfg.schema_version) {
        ESP_LOGE(TAG, "payload schema %d too new (current %u)",
                 (int)dev_sch->valuedouble, s.cfg.schema_version);
        cJSON_Delete(root);
        set_status(PC_STATUS_ERR, PC_ERR_SCHEMA);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!cJSON_IsObject(payload)) {
        ESP_LOGE(TAG, "missing or invalid 'payload' object");
        cJSON_Delete(root);
        set_status(PC_STATUS_ERR, PC_ERR_PARSE);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "importing from device='%s' schema=%d",
             device_j->valuestring, (int)dev_sch->valuedouble);

    esp_err_t err = s.cfg.deserialize(payload);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "deserialize failed: %d", err);
        set_status(PC_STATUS_ERR, PC_ERR_APPLY);
        return err;
    }

    set_status(PC_STATUS_OK, PC_ERR_NONE);
    ESP_LOGI(TAG, "import complete");
    return ESP_OK;
}

/* Called by Zigbee transport to commit the assembled chunk buffer */
esp_err_t portable_config_commit_import(void)
{
    s.buf_len = s.write_pos;
    if (s.buf_len == 0) {
        set_status(PC_STATUS_ERR, PC_ERR_PARSE);
        return ESP_ERR_INVALID_STATE;
    }
    s.buf[s.buf_len] = '\0';
    return portable_config_do_import(s.buf, s.buf_len);
}

void portable_config_set_endpoint(uint8_t ep)
{
    s.endpoint = ep;
}

uint8_t portable_config_get_ep(void)
{
    return s.endpoint;
}

uint16_t portable_config_get_cluster_id(void)
{
    return s.cluster_id;
}

void portable_config_reset(void)
{
    reset_transfer();
}

const char *portable_config_get_buf(size_t *len_out)
{
    if (len_out) *len_out = s.buf_len;
    return s.buf;
}

char *portable_config_get_buf_rw(void)
{
    return s.buf;
}

void portable_config_set_write_pos(size_t pos)
{
    if (pos > s.write_pos) s.write_pos = pos;
}

size_t portable_config_get_chunk_count(void)
{
    return s.chunk_count;
}

uint8_t portable_config_get_status(void)
{
    return s.status;
}

/* ---- SSE status serializer ---- */

int portable_config_get_status_json(char *buf, size_t buf_len)
{
    static const char *status_str[] = {"idle","exporting","importing","ok","error"};
    const char *st = (s.status < 5) ? status_str[s.status] : "error";
    return snprintf(buf, buf_len,
        "{\"status\":\"%s\",\"error_code\":%u,\"chunk_count\":%u}",
        st, s.error_code, s.chunk_count);
}
