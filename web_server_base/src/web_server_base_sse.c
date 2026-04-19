// SPDX-License-Identifier: MIT
/**
 * web_server_base_sse.c — Server-Sent Events (SSE) support for web_server_base.
 *
 * Architecture:
 *  - One registered endpoint (e.g. "/api/events") with N topics.
 *  - Per connected client: an async httpd_req_t + a dedicated FreeRTOS task.
 *  - web_server_base_sse_notify(topic): increments the topic version and wakes
 *    all client tasks via xTaskNotifyGive. Safe from any task context.
 *  - Client task: sends the serialized payload for each changed topic, then
 *    blocks on xTaskNotifyWait. Sends a keepalive comment on timeout.
 */
#include "web_server_base.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wsb_sse";

#define SSE_MAX_CLIENTS    2
#define SSE_MAX_TOPICS     8
#define SSE_BUF_SIZE       4096
#define SSE_KEEPALIVE_MS   15000
#define SSE_TASK_STACK     4096
#define SSE_TASK_PRIORITY  (tskIDLE_PRIORITY + 1)

/* Internal getter — defined in web_server_base.c */
extern httpd_handle_t web_server_base_get_server(void);

/* ================================================================== */
/*  State                                                              */
/* ================================================================== */

typedef struct {
    const char *const *topics;
    int                topic_count;
    wsb_sse_serializer_t serializer;
    uint32_t           versions[SSE_MAX_TOPICS];
    SemaphoreHandle_t  mutex;
} wsb_ep_t;

typedef struct {
    wsb_ep_t      *ep;
    httpd_req_t   *req;       /* async req handle — valid while alive */
    TaskHandle_t   task;
    uint32_t       last_seen[SSE_MAX_TOPICS];
    bool           alive;
} wsb_client_t;

static wsb_ep_t       s_ep;
static bool           s_registered  = false;
static wsb_client_t   s_clients[SSE_MAX_CLIENTS];
static SemaphoreHandle_t s_client_mutex;
static bool           s_init_done   = false;

/* ================================================================== */
/*  SSE framing                                                        */
/* ================================================================== */

static esp_err_t send_chunk(httpd_req_t *req, const char *buf, size_t len)
{
    return httpd_resp_send_chunk(req, buf, (ssize_t)len);
}

static esp_err_t send_event(httpd_req_t *req, const char *topic, const char *data, int data_len)
{
    char hdr[72];
    int hlen = snprintf(hdr, sizeof(hdr), "event: %s\ndata: ", topic);
    if (send_chunk(req, hdr, (size_t)hlen) != ESP_OK) return ESP_FAIL;
    if (send_chunk(req, data, (size_t)data_len) != ESP_OK) return ESP_FAIL;
    if (send_chunk(req, "\n\n", 2) != ESP_OK) return ESP_FAIL;
    return ESP_OK;
}

/* ================================================================== */
/*  Per-client task                                                    */
/* ================================================================== */

static void client_task(void *arg)
{
    wsb_client_t *c  = (wsb_client_t *)arg;
    wsb_ep_t     *ep = c->ep;
    char *buf = malloc(SSE_BUF_SIZE);

    if (!buf) {
        ESP_LOGE(TAG, "OOM: SSE buffer");
        goto done;
    }

    /* Set SSE response headers — sent on first chunk */
    httpd_resp_set_type(c->req, "text/event-stream");
    httpd_resp_set_hdr(c->req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(c->req, "Connection",    "keep-alive");
    httpd_resp_set_hdr(c->req, "Access-Control-Allow-Origin", "*");

    /* Flush headers and signal connection established */
    if (send_chunk(c->req, ": connected\n\n", 13) != ESP_OK) goto done;

    /* Send initial state for every topic */
    for (int i = 0; i < ep->topic_count; i++) {
        xSemaphoreTake(ep->mutex, portMAX_DELAY);
        c->last_seen[i] = ep->versions[i];
        xSemaphoreGive(ep->mutex);

        int n = ep->serializer(ep->topics[i], buf, SSE_BUF_SIZE);
        if (n > 0 && send_event(c->req, ep->topics[i], buf, n) != ESP_OK) goto done;
    }

    /* Event loop */
    for (;;) {
        BaseType_t notified = xTaskNotifyWait(0, UINT32_MAX, NULL,
                                              pdMS_TO_TICKS(SSE_KEEPALIVE_MS));
        if (!c->alive) break;

        if (notified == pdFALSE) {
            /* Timeout — keepalive comment */
            if (send_chunk(c->req, ": keepalive\n\n", 13) != ESP_OK) goto done;
        } else {
            /* Send any changed topics */
            for (int i = 0; i < ep->topic_count; i++) {
                xSemaphoreTake(ep->mutex, portMAX_DELAY);
                uint32_t ver = ep->versions[i];
                xSemaphoreGive(ep->mutex);

                if (ver == c->last_seen[i]) continue;
                c->last_seen[i] = ver;

                int n = ep->serializer(ep->topics[i], buf, SSE_BUF_SIZE);
                if (n > 0 && send_event(c->req, ep->topics[i], buf, n) != ESP_OK) goto done;
            }
        }
    }

done:
    free(buf);
    ESP_LOGI(TAG, "SSE client disconnecting");

    httpd_req_async_handler_complete(c->req);

    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    c->alive = false;
    c->task  = NULL;
    c->req   = NULL;
    xSemaphoreGive(s_client_mutex);

    vTaskDelete(NULL);
}

/* ================================================================== */
/*  URI handler — GET /api/events                                     */
/* ================================================================== */

static esp_err_t handle_sse(httpd_req_t *req)
{
    if (!s_registered) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SSE not configured");
        return ESP_OK;
    }

    /* Claim an async req handle before touching client state */
    httpd_req_t *async_req;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async init failed");
        return ESP_OK;
    }

    /* Find a free client slot */
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    wsb_client_t *c = NULL;
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (!s_clients[i].alive) { c = &s_clients[i]; break; }
    }
    if (!c) {
        xSemaphoreGive(s_client_mutex);
        httpd_resp_set_status(async_req, "503 Service Unavailable");
        httpd_resp_sendstr(async_req, "Too many SSE clients");
        httpd_req_async_handler_complete(async_req);
        ESP_LOGW(TAG, "SSE: no free client slots");
        return ESP_OK;
    }
    memset(c, 0, sizeof(*c));
    c->ep    = &s_ep;
    c->req   = async_req;
    c->alive = true;
    xSemaphoreGive(s_client_mutex);

    if (xTaskCreate(client_task, "sse_client", SSE_TASK_STACK, c,
                    SSE_TASK_PRIORITY, &c->task) != pdPASS) {
        xSemaphoreTake(s_client_mutex, portMAX_DELAY);
        c->alive = false;
        xSemaphoreGive(s_client_mutex);
        httpd_req_async_handler_complete(async_req);
        ESP_LOGE(TAG, "SSE: task create failed");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "SSE client connected");
    return ESP_OK;
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

static void ensure_init(void)
{
    if (s_init_done) return;
    s_ep.mutex      = xSemaphoreCreateMutex();
    s_client_mutex  = xSemaphoreCreateMutex();
    s_init_done     = true;
}

esp_err_t web_server_base_sse_register(const char *uri,
                                       const char *const *topics,
                                       wsb_sse_serializer_t serializer)
{
    httpd_handle_t server = web_server_base_get_server();
    if (!server)     return ESP_ERR_INVALID_STATE;
    if (!uri || !topics || !serializer) return ESP_ERR_INVALID_ARG;

    ensure_init();

    /* Count topics */
    int count = 0;
    while (topics[count] && count < SSE_MAX_TOPICS) count++;

    s_ep.topics      = topics;
    s_ep.topic_count = count;
    s_ep.serializer  = serializer;
    memset(s_ep.versions, 0, sizeof(s_ep.versions));
    s_registered     = true;

    httpd_uri_t h = {
        .uri     = uri,
        .method  = HTTP_GET,
        .handler = handle_sse,
    };
    esp_err_t err = httpd_register_uri_handler(server, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SSE register failed: %s", esp_err_to_name(err));
        s_registered = false;
    } else {
        ESP_LOGI(TAG, "SSE endpoint registered: %s (%d topics)", uri, count);
    }
    return err;
}

void web_server_base_sse_notify(const char *topic)
{
    if (!s_registered || !topic) return;

    /* Find topic index */
    int idx = -1;
    for (int i = 0; i < s_ep.topic_count; i++) {
        if (strcmp(s_ep.topics[i], topic) == 0) { idx = i; break; }
    }
    if (idx < 0) return;

    /* Increment topic version */
    xSemaphoreTake(s_ep.mutex, portMAX_DELAY);
    s_ep.versions[idx]++;
    xSemaphoreGive(s_ep.mutex);

    /* Wake every live client */
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (s_clients[i].alive && s_clients[i].task) {
            xTaskNotifyGive(s_clients[i].task);
        }
    }
    xSemaphoreGive(s_client_mutex);
}
