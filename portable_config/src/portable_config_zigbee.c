// SPDX-License-Identifier: MIT
#include "portable_config.h"

#include "esp_log.h"
#include "esp_zigbee_core.h"
#include <string.h>

static const char *TAG = "pc_zb";

/* Internal accessors from portable_config.c */
extern uint8_t  portable_config_get_ep(void);
extern uint16_t portable_config_get_cluster_id(void);
extern uint8_t  portable_config_get_status(void);
extern const char *portable_config_get_buf(size_t *len_out);
extern char    *portable_config_get_buf_rw(void);
extern void     portable_config_set_write_pos(size_t pos);
extern void     portable_config_reset(void);
extern esp_err_t portable_config_do_export(void);
extern esp_err_t portable_config_commit_import(void);

/* ZCL backing storage for cluster attributes */
static uint16_t s_chunk_idx   = 0;
static uint16_t s_chunk_count = 0;
static uint8_t  s_trigger     = 0;
static uint8_t  s_status      = PC_STATUS_IDLE;
static uint8_t  s_error_code  = PC_ERR_NONE;

/* chunk_data: ZCL CHAR_STRING — first byte = length, then up to 64 bytes of payload */
static uint8_t s_chunk_data_zcl[PC_CHUNK_SIZE + 2];

/* ---- Add portable-config attrs to an existing custom cluster attr list ---- */

esp_err_t portable_config_add_attrs(esp_zb_attribute_list_t *attr_list)
{
    if (!attr_list) return ESP_ERR_INVALID_ARG;

    s_chunk_data_zcl[0] = 0x00;

    esp_zb_custom_cluster_add_custom_attr(attr_list, PC_ATTR_CHUNK_IDX,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_chunk_idx);

    esp_zb_custom_cluster_add_custom_attr(attr_list, PC_ATTR_CHUNK_DATA,
        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        s_chunk_data_zcl);

    esp_zb_custom_cluster_add_custom_attr(attr_list, PC_ATTR_CHUNK_COUNT,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_chunk_count);

    esp_zb_custom_cluster_add_custom_attr(attr_list, PC_ATTR_TRIGGER,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_trigger);

    esp_zb_custom_cluster_add_custom_attr(attr_list, PC_ATTR_STATUS,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_status);

    esp_zb_custom_cluster_add_custom_attr(attr_list, PC_ATTR_ERROR_CODE,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &s_error_code);

    return ESP_OK;
}

/* ---- Export: update chunk_data ZCL attr for the requested chunk index ---- */

static void update_chunk_data_for_export(uint16_t idx)
{
    uint8_t ep = portable_config_get_ep();
    if (!ep) return;

    size_t buf_len;
    const char *buf = portable_config_get_buf(&buf_len);
    if (!buf || buf_len == 0) {
        s_chunk_data_zcl[0] = 0;
        esp_zb_zcl_set_attribute_val(ep, portable_config_get_cluster_id(),
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, PC_ATTR_CHUNK_DATA, s_chunk_data_zcl, false);
        return;
    }

    size_t offset = (size_t)idx * PC_CHUNK_SIZE;
    if (offset >= buf_len) {
        s_chunk_data_zcl[0] = 0;
    } else {
        size_t chunk_len = buf_len - offset;
        if (chunk_len > PC_CHUNK_SIZE) chunk_len = PC_CHUNK_SIZE;
        s_chunk_data_zcl[0] = (uint8_t)chunk_len;
        memcpy(s_chunk_data_zcl + 1, buf + offset, chunk_len);
    }

    esp_zb_zcl_set_attribute_val(ep, portable_config_get_cluster_id(),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, PC_ATTR_CHUNK_DATA, s_chunk_data_zcl, false);
}

/* ---- Deferred import callback (runs on next ZBoss tick, after attr write returns) ---- */

/* Commit import is deferred out of the attribute write callback because the project's
 * deserialize callback (config_api_apply_all) may issue blocking UART commands and call
 * multiple Zigbee stack functions (esp_zb_zcl_set_attribute_val, esp_zb_scheduler_alarm).
 * Running all of that inline inside a ZBoss attribute write callback exceeds ZBoss's
 * expected callback duration and causes zb_scheduler.c assertion failures. */
static void deferred_commit_import_cb(uint8_t param)
{
    (void)param;
    portable_config_commit_import();
}

/* ---- Attribute write handler (called from project's zigbee_attr_handler.c) ---- */

esp_err_t portable_config_handle_attr_write(uint16_t attr_id, void *val)
{
    if (!val) return ESP_ERR_INVALID_ARG;

    switch (attr_id) {

    case PC_ATTR_CHUNK_IDX: {
        s_chunk_idx = *(uint16_t *)val;
        if (portable_config_get_status() == PC_STATUS_EXPORTING) {
            update_chunk_data_for_export(s_chunk_idx);
        }
        return ESP_OK;
    }

    case PC_ATTR_CHUNK_DATA: {
        /* ZCL CHAR_STRING: val[0] = length byte, val[1..] = ASCII data */
        uint8_t *zcl_str  = (uint8_t *)val;
        uint8_t  chunk_len = zcl_str[0];
        if (chunk_len > PC_CHUNK_SIZE) chunk_len = PC_CHUNK_SIZE;

        char *buf = portable_config_get_buf_rw();
        if (!buf) return ESP_ERR_INVALID_STATE;

        size_t write_pos = (size_t)s_chunk_idx * PC_CHUNK_SIZE;
        if (write_pos + chunk_len > PC_MAX_BUNDLE_SIZE - 1) {
            ESP_LOGE(TAG, "import overflow at pos=%u len=%u", (unsigned)write_pos, chunk_len);
            return ESP_ERR_INVALID_SIZE;
        }

        memcpy(buf + write_pos, zcl_str + 1, chunk_len);
        portable_config_set_write_pos(write_pos + chunk_len);
        ESP_LOGD(TAG, "chunk %u written (%u bytes at offset %u)",
                 s_chunk_idx, chunk_len, (unsigned)write_pos);
        return ESP_OK;
    }

    case PC_ATTR_TRIGGER: {
        uint8_t trig = *(uint8_t *)val;
        ESP_LOGI(TAG, "trigger=%u", trig);
        switch (trig) {
        case 1: return portable_config_do_export();
        case 2:
            /* Defer to next ZBoss tick — the deserialize callback is too heavy for inline execution */
            esp_zb_scheduler_alarm(deferred_commit_import_cb, 0, 0);
            return ESP_OK;
        case 3: portable_config_reset(); return ESP_OK;
        default:
            ESP_LOGW(TAG, "unknown trigger %u", trig);
            return ESP_ERR_INVALID_ARG;
        }
    }

    default:
        return ESP_OK;
    }
}
