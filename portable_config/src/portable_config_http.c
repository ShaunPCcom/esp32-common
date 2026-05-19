// SPDX-License-Identifier: MIT
/**
 * portable_config_http.c — HTTP import/export endpoints (ESP32-C6 only).
 *
 * GET  /api/backup  → export config as a JSON file download
 * POST /api/backup  → import config from JSON body (up to PC_MAX_BUNDLE_SIZE bytes)
 */
#include "portable_config.h"

#ifdef CONFIG_IDF_TARGET_ESP32C6

#include "web_server_base.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "pc_http";

/* Internal accessors from portable_config.c */
extern esp_err_t    portable_config_do_export(void);
extern esp_err_t    portable_config_do_import(const char *json, size_t len);
extern const char  *portable_config_get_buf(size_t *len_out);

/* ---- Body reader with custom size cap ---- */

static char *read_body_pc(httpd_req_t *req)
{
    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > PC_MAX_BUNDLE_SIZE) {
        return NULL;
    }
    char *buf = malloc(content_len + 1);
    if (!buf) return NULL;

    int received = 0;
    int remaining = (int)content_len;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf + received, remaining);
        if (ret <= 0) {
            free(buf);
            return NULL;
        }
        received  += ret;
        remaining -= ret;
    }
    buf[received] = '\0';
    return buf;
}

/* ---- Helper: send JSON response ---- */

static void send_json_resp(httpd_req_t *req, int status_code, const char *status_str,
                           const char *error_msg)
{
    char status_line[32];
    snprintf(status_line, sizeof(status_line), "%d %s", status_code,
             status_code == 200 ? "OK" :
             status_code == 400 ? "Bad Request" :
             status_code == 413 ? "Content Too Large" : "Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status_line);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", status_str);
    if (error_msg) cJSON_AddStringToObject(resp, "error", error_msg);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (json) {
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"error\"}");
    }
}

/* ================================================================== */
/*  GET /api/backup — export config as downloadable JSON file         */
/* ================================================================== */

static esp_err_t handle_get_backup(httpd_req_t *req)
{
    esp_err_t err = portable_config_do_export();
    if (err != ESP_OK) {
        send_json_resp(req, 500, "error", "export failed");
        return ESP_OK;
    }

    size_t len;
    const char *json = portable_config_get_buf(&len);
    if (!json || len == 0) {
        send_json_resp(req, 500, "error", "empty bundle");
        return ESP_OK;
    }

    /* Build a timestamped filename */
    char filename_hdr[80];
    time_t now = time(NULL);
    snprintf(filename_hdr, sizeof(filename_hdr),
             "attachment; filename=\"backup-%lld.json\"", (long long)now);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", filename_hdr);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, json, (ssize_t)len);

    ESP_LOGI(TAG, "export sent: %u bytes", (unsigned)len);
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/backup — import config from JSON body                   */
/* ================================================================== */

static esp_err_t handle_post_backup(httpd_req_t *req)
{
    if (req->content_len > PC_MAX_BUNDLE_SIZE) {
        send_json_resp(req, 413, "error", "body exceeds 8 KB limit");
        return ESP_OK;
    }

    char *body = read_body_pc(req);
    if (!body) {
        send_json_resp(req, 400, "error", "failed to read body");
        return ESP_OK;
    }

    size_t len = strlen(body);
    esp_err_t err = portable_config_do_import(body, len);
    free(body);

    if (err != ESP_OK) {
        const char *msg;
        switch (err) {
        case ESP_ERR_NOT_SUPPORTED: msg = "unsupported schema version"; break;
        case ESP_ERR_INVALID_ARG:   msg = "device tag mismatch or bad JSON"; break;
        case ESP_ERR_INVALID_SIZE:  msg = "bundle too large"; break;
        default:                    msg = "import failed"; break;
        }
        send_json_resp(req, 400, "error", msg);
        return ESP_OK;
    }

    /* Notify SSE "backup" topic so open web UI refreshes */
    web_server_base_sse_notify("backup");

    send_json_resp(req, 200, "ok", NULL);
    ESP_LOGI(TAG, "import applied via HTTP");
    return ESP_OK;
}

/* ================================================================== */
/*  Registration                                                       */
/* ================================================================== */

esp_err_t portable_config_register_http(void)
{
    esp_err_t err;
    err = web_server_base_register("/api/backup", HTTP_GET,  handle_get_backup,  false);
    if (err != ESP_OK) return err;
    err = web_server_base_register("/api/backup", HTTP_POST, handle_post_backup, false);
    return err;
}

#else  /* !CONFIG_IDF_TARGET_ESP32C6 */

esp_err_t portable_config_register_http(void)
{
    return ESP_OK; /* no-op on H2 */
}

#endif /* CONFIG_IDF_TARGET_ESP32C6 */
