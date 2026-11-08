// SPDX-License-Identifier: MIT
#include "web_server_base.h"
#include "wifi_manager.h"
#include "ota_check.h"
#include "crash_diag.h"
#include "zigbee_ota.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef CONFIG_IDF_TARGET_ESP32C6
extern esp_err_t ota_upload_transport_flash(httpd_req_t *req);
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wsb";
static httpd_handle_t s_server = NULL;
static web_server_base_config_t s_cfg_copy;
static const web_server_base_config_t *s_cfg = NULL;
static char *s_setup_html = NULL;

#define MAX_BODY_LEN 4096

/* ================================================================== */
/*  Setup page template (served in AP mode, before SPIFFS UI is ready) */
/* ================================================================== */

/* Three %s substitutions: device_name (title), device_name (h1), firmware_version */
static const char SETUP_HTML_TMPL[] =
    "<!DOCTYPE html>"
    "<html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>%s Setup</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px;color:#333}"
    "h1{margin-bottom:4px}p{margin-top:4px;color:#555}"
    "label{display:block;margin-top:14px;font-weight:600;font-size:.9em}"
    "input,select{width:100%%;padding:8px;margin-top:4px;box-sizing:border-box;"
    "border:1px solid #ccc;border-radius:4px;font-size:1em;background:#fff}"
    ".row{display:flex;gap:8px;margin-top:4px;align-items:stretch}"
    ".row select{flex:1;margin-top:0;width:auto}"
    ".scan{padding:8px 12px;background:#f5f5f5;color:#333;border:1px solid #ccc;"
    "border-radius:4px;font-size:1em;cursor:pointer;white-space:nowrap}"
    ".scan:active{background:#ddd}"
    "button.sub{margin-top:20px;padding:12px;background:#0070f3;color:#fff;"
    "border:none;border-radius:6px;font-size:1em;cursor:pointer;width:100%%}"
    "button.sub:active{background:#0051cc}"
    ".info{background:#f5f7ff;border-radius:6px;padding:12px;margin-top:20px;"
    "font-size:.85em;line-height:1.8}"
    ".msg{padding:10px;border-radius:4px;margin-top:16px;display:none}"
    ".ok{background:#d4edda;color:#155724}.err{background:#f8d7da;color:#721c24}"
    ".hint{color:#888;font-size:.8em;margin-top:3px}"
    "</style></head><body>"
    "<h1>%s Setup</h1>"
    "<p>Configure this device for WiFi access.</p>"
    "<label>Device Hostname</label>"
    "<input id=\"h\" type=\"text\" placeholder=\"e.g. my-device\" maxlength=\"32\""
    " autocomplete=\"off\" autocorrect=\"off\" spellcheck=\"false\">"
    "<p class=\"hint\">Accessible at http://&lt;hostname&gt;/ after setup. Letters, numbers, hyphens only.</p>"
    "<label>WiFi Network</label>"
    "<div class=\"row\">"
    "<select id=\"net\" onchange=\"onNet()\"><option value=\"\">Scanning...</option></select>"
    "<button type=\"button\" class=\"scan\" onclick=\"scan()\">&#8635; Scan</button>"
    "</div>"
    "<input id=\"s\" type=\"text\" placeholder=\"Hidden network SSID\" style=\"display:none\""
    " autocomplete=\"off\" autocorrect=\"off\" spellcheck=\"false\">"
    "<label>Password</label>"
    "<input id=\"p\" type=\"password\" placeholder=\"Leave blank for open networks\">"
    "<button class=\"sub\" onclick=\"save()\">Save &amp; Connect</button>"
    "<div id=\"msg\" class=\"msg\"></div>"
    "<div class=\"info\">"
    "<b>Firmware:</b> %s<br>"
    "<b>API:</b> <a href=\"/api/status\">/api/status</a>"
    "</div>"
    "<script>"
    "window.onload=function(){scan();};"
    "function scan(){"
    "var sel=document.getElementById('net');"
    "sel.innerHTML='<option value=\"\">Scanning...</option>';"
    "sel.disabled=true;"
    "document.getElementById('s').style.display='none';"
    "fetch('/api/wifi-scan')"
    ".then(function(r){return r.json();})"
    ".then(function(nets){"
    "sel.innerHTML='';"
    "nets.forEach(function(n){"
    "var o=document.createElement('option');"
    "o.value=n.ssid;"
    "o.textContent=n.ssid+' ('+n.rssi+' dBm)';"
    "sel.appendChild(o);"
    "});"
    "var h=document.createElement('option');"
    "h.value='__hidden__';h.textContent='-- Hidden network --';"
    "sel.appendChild(h);"
    "sel.disabled=false;onNet();"
    "}).catch(function(){"
    "sel.innerHTML='';"
    "var h=document.createElement('option');"
    "h.value='__hidden__';h.textContent='-- Enter manually --';"
    "sel.appendChild(h);"
    "sel.disabled=false;onNet();"
    "});}"
    "function onNet(){"
    "var v=document.getElementById('net').value;"
    "document.getElementById('s').style.display=(v==='__hidden__'?'block':'none');}"
    "function save(){"
    "var sel=document.getElementById('net');"
    "var ssid=sel.value==='__hidden__'?"
    "document.getElementById('s').value.trim():sel.value;"
    "var h=document.getElementById('h').value.trim(),"
    "p=document.getElementById('p').value,"
    "m=document.getElementById('msg');"
    "if(!h){alert('Hostname is required');return;}"
    "if(!/^[A-Za-z0-9\\-]+$/.test(h)){alert('Hostname: letters, numbers and hyphens only');return;}"
    "if(!ssid){alert('SSID is required');return;}"
    "fetch('/api/wifi',{method:'POST',"
    "headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({ssid:ssid,password:p,hostname:h})})"
    ".then(function(r){return r.json();})"
    ".then(function(d){"
    "var url=d.hostname?'http://'+d.hostname+'/':null;"
    "m.innerHTML=url?"
    "'Credentials saved \u2014 Please connect to your WiFi and go to"
    " <a href=\"'+url+'\">'+url+'</a>'"
    ":(d.message||'Saved');"
    "m.className='msg ok';m.style.display='block';"
    "}).catch(function(){"
    "m.textContent='Request failed - check connection';"
    "m.className='msg err';m.style.display='block';});}"
    "</script>"
    "</body></html>";

/* ================================================================== */
/*  Web assets — embedded in firmware, synced to SPIFFS on boot       */
/* ================================================================== */

static bool s_spiffs_ok = false;

static void mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/www",
        .partition_label        = "www",
        .max_files              = 6,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
    } else {
        s_spiffs_ok = true;
    }
}

static void sync_web_assets(void)
{
    if (!s_spiffs_ok || !s_cfg) return;

    nvs_handle_t nvs = 0;
    bool needs_update = true;
    if (nvs_open(s_cfg->nvs_namespace, NVS_READWRITE, &nvs) == ESP_OK) {
        char stored[16] = {0};
        size_t len = sizeof(stored);
        if (nvs_get_str(nvs, "web_asset_ver", stored, &len) == ESP_OK &&
            strcmp(stored, s_cfg->firmware_version) == 0) {
            needs_update = false;
        }
    }

    if (!needs_update) {
        ESP_LOGI(TAG, "Web assets current (%s)", s_cfg->firmware_version);
        if (nvs) nvs_close(nvs);
        return;
    }

    ESP_LOGI(TAG, "Updating web assets to %s", s_cfg->firmware_version);

    struct {
        const char    *path;
        const uint8_t *start;
        size_t         size;
    } files[] = {
        { "/www/index.html", s_cfg->index_html_start, s_cfg->index_html_size },
        { "/www/app.js",     s_cfg->app_js_start,     s_cfg->app_js_size     },
        { "/www/style.css",  s_cfg->style_css_start,  s_cfg->style_css_size  },
    };

    bool ok = true;
    for (int i = 0; i < 3; i++) {
        size_t len = files[i].size;
        /* TEXT mode appends a null byte — strip it when writing to SPIFFS */
        if (len > 0 && files[i].start[len - 1] == '\0') len--;
        FILE *f = fopen(files[i].path, "w");
        if (!f) { ESP_LOGE(TAG, "Cannot write %s", files[i].path); ok = false; continue; }
        size_t written = fwrite(files[i].start, 1, len, f);
        fclose(f);
        if (written != len) { ESP_LOGE(TAG, "Short write %s", files[i].path); ok = false; }
    }

    if (ok && nvs) {
        nvs_set_str(nvs, "web_asset_ver", s_cfg->firmware_version);
        nvs_commit(nvs);
        ESP_LOGI(TAG, "Web assets updated");
    }
    if (nvs) nvs_close(nvs);
}

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

static char *read_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY_LEN) {
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + received,
                               req->content_len - (size_t)received);
        if (r <= 0) { free(buf); return NULL; }
        received += r;
    }
    buf[received] = '\0';
    return buf;
}

static void send_json(httpd_req_t *req, int http_status, cJSON *json)
{
    char *str = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (http_status == 400) {
        httpd_resp_set_status(req, "400 Bad Request");
    } else if (http_status == 500) {
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_sendstr(req, str ? str : "{}");
    free(str);
}

static void send_redirect(httpd_req_t *req, const char *location)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_sendstr(req, "");
}

static esp_err_t serve_file(httpd_req_t *req, const char *path,
                             const char *content_type)
{
    FILE *f = fopen(path, "r");
    if (!f) { httpd_resp_send_404(req); return ESP_OK; }
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[2048];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ================================================================== */
/*  Static asset handlers                                              */
/* ================================================================== */

static esp_err_t handle_root(httpd_req_t *req)
{
    if (wifi_manager_is_ap_mode()) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req, s_setup_html ? s_setup_html : "<h1>Setup</h1>");
        return ESP_OK;
    }
    return serve_file(req, "/www/index.html", "text/html");
}

static esp_err_t handle_app_js(httpd_req_t *req)
{
    return serve_file(req, "/www/app.js", "application/javascript");
}

static esp_err_t handle_style_css(httpd_req_t *req)
{
    return serve_file(req, "/www/style.css", "text/css");
}

/* ================================================================== */
/*  GET /wifi.js — shared WiFi tab logic                              */
/* ================================================================== */

static const char WIFI_JS[] =
    "(function(){'use strict';"
    "function notify(m,t){if(typeof window.toast==='function')window.toast(m,t||'');else alert(m);}"
    "function wfScan(){"
    "var s=document.getElementById('wifi-net-select');if(!s)return;"
    "s.innerHTML='<option value=\"\">Scanning\u2026</option>';s.disabled=true;"
    "fetch('/api/wifi-scan').then(function(r){return r.json();}).then(function(n){"
    "s.innerHTML='';"
    "(n||[]).sort(function(a,b){return b.rssi-a.rssi;}).forEach(function(x){"
    "var o=document.createElement('option');o.value=x.ssid;"
    "o.textContent=x.ssid+'  ('+x.rssi+' dBm)';s.appendChild(o);});"
    "addOpts(s);s.disabled=false;wfOnChange();"
    "}).catch(function(){s.innerHTML='';addOpts(s);s.disabled=false;wfOnChange();});}"
    "function addOpts(s){"
    "var m=document.createElement('option');m.value='__manual__';"
    "m.textContent='\u2014 Enter SSID manually \u2014';s.appendChild(m);"
    "var r=document.createElement('option');r.value='__reset__';"
    "r.textContent='\u2014 Reset WiFi / AP mode \u2014';s.appendChild(r);}"
    "function wfOnChange(){"
    "var v=(document.getElementById('wifi-net-select')||{}).value||'';"
    "var mr=document.getElementById('wifi-manual-row');"
    "var pr=document.getElementById('wifi-password-row');"
    "var hr=document.getElementById('wifi-hostname-row');"
    "var b=document.getElementById('wifi-action-btn');"
    "if(mr)mr.style.display=v==='__manual__'?'':'none';"
    "if(pr)pr.style.display=v==='__reset__'?'none':'';"
    "if(hr)hr.style.display=v==='__reset__'?'none':'';"
    "if(b){"
    "if(v==='__reset__'){"
    "b.className=b.dataset.resetClass||'btn danger';"
    "b.textContent='\u21ba Reset WiFi';"
    "}else{"
    "b.className=b.dataset.connectClass||'btn primary';"
    "b.textContent='\u2713 Connect';}}}"
    "function wfAction(){"
    "var v=(document.getElementById('wifi-net-select')||{}).value||'';"
    "if(v==='__reset__'){"
    "if(!confirm('Clear WiFi credentials and reboot to AP mode?'))return;"
    "fetch('/api/wifi-reset',{method:'POST'})"
    ".then(function(){notify('Rebooting in AP mode\u2026','ok');})"
    ".catch(function(){notify('Request failed','err');});return;}"
    "var ssid=v==='__manual__'"
    "?((document.getElementById('wifi-manual-ssid')||{}).value||'').trim():v;"
    "var pass=(document.getElementById('wifi-password')||{}).value||'';"
    "var host=((document.getElementById('wifi-hostname')||{}).value||'').trim();"
    "if(!ssid){notify('Select or enter a network SSID','err');return;}"
    "if(!host){notify('Hostname is required','err');return;}"
    "if(!/^[A-Za-z0-9-]+$/.test(host)){notify('Hostname: letters, numbers and hyphens only','err');return;}"
    "fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({ssid:ssid,password:pass,hostname:host})})"
    ".then(function(r){return r.json();})"
    ".then(function(d){"
    "if(d.status==='ok'){"
    "notify('Connecting to '+ssid+'\u2026','ok');"
    "if(d.hostname)setTimeout(function(){window.location.href='http://'+d.hostname+'/';},3000);"
    "}else notify('Error: '+(d.error||'unknown'),'err');})"
    ".catch(function(){notify('Request failed','err');});}"
    "document.addEventListener('DOMContentLoaded',function(){"
    "var h=document.getElementById('wifi-hostname');"
    "if(h&&window.location.hostname)h.value=window.location.hostname;"
    "setTimeout(wfScan,500);});"
    "window.wfScan=wfScan;window.wfOnChange=wfOnChange;window.wfAction=wfAction;"
    "})();";

static esp_err_t handle_wifi_js(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, WIFI_JS);
    return ESP_OK;
}

/* ================================================================== */
/*  Captive portal probes                                              */
/* ================================================================== */

static esp_err_t handle_generate_204(httpd_req_t *req)
{
    send_redirect(req, "http://192.168.4.1/");
    return ESP_OK;
}

static esp_err_t handle_hotspot_detect(httpd_req_t *req)
{
    send_redirect(req, "http://192.168.4.1/");
    return ESP_OK;
}

static esp_err_t handle_ncsi(httpd_req_t *req)
{
    send_redirect(req, "http://192.168.4.1/");
    return ESP_OK;
}

/* ================================================================== */
/*  GET /api/wifi-scan                                                 */
/* ================================================================== */

#define WIFI_SCAN_MAX_APS  20

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", esp_err_to_name(err));
        send_json(req, 500, e);
        cJSON_Delete(e);
        return ESP_OK;
    }

    uint16_t ap_count = WIFI_SCAN_MAX_APS;
    wifi_ap_record_t *ap_list = calloc(WIFI_SCAN_MAX_APS, sizeof(wifi_ap_record_t));
    if (!ap_list) { esp_wifi_clear_ap_list(); httpd_resp_send_500(req); return ESP_OK; }
    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        if (ap_list[i].ssid[0] == '\0') continue;
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((char *)ap_list[i].ssid, (char *)ap_list[j].ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", ap_list[i].rssi);
        cJSON_AddBoolToObject(ap, "open", ap_list[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, ap);
    }
    free(ap_list);
    send_json(req, 200, arr);
    cJSON_Delete(arr);
    return ESP_OK;
}

/* ================================================================== */
/*  GET /api/status                                                    */
/* ================================================================== */

static esp_err_t handle_get_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_500(req); return ESP_OK; }

    cJSON_AddStringToObject(root, "firmware",   s_cfg ? s_cfg->firmware_version : "");
    cJSON_AddNumberToObject(root, "uptime_sec", (double)(int64_t)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(root, "free_heap",  (double)esp_get_free_heap_size());

    static const char *wifi_state_names[] = {
        [WIFI_MGR_STATE_INIT]           = "init",
        [WIFI_MGR_STATE_AP]             = "ap",
        [WIFI_MGR_STATE_STA_CONNECTING] = "connecting",
        [WIFI_MGR_STATE_STA_CONNECTED]  = "connected",
        [WIFI_MGR_STATE_STA_FAILED]     = "failed",
    };
    wifi_mgr_state_t ws = wifi_manager_get_state();
    cJSON_AddStringToObject(root, "wifi",
        (ws < (wifi_mgr_state_t)(sizeof(wifi_state_names) / sizeof(wifi_state_names[0])))
            ? wifi_state_names[ws] : "unknown");

    /* SSID of connected AP (STA mode only) */
    if (ws == WIFI_MGR_STATE_STA_CONNECTED) {
        wifi_ap_record_t ap_info = {};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            cJSON_AddStringToObject(root, "ssid", (char *)ap_info.ssid);
        }
    }
    /* Device hostname */
    char hostname_buf[33] = {};
    if (wifi_manager_get_hostname(hostname_buf, sizeof(hostname_buf))) {
        cJSON_AddStringToObject(root, "hostname", hostname_buf);
    }

    send_json(req, 200, root);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ================================================================== */
/*  GET /api/diag                                                      */
/* ================================================================== */

static esp_err_t handle_get_diag(httpd_req_t *req)
{
    crash_diag_data_t d;
    crash_diag_get_data(&d);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "boot_count",     (double)d.boot_count);
    cJSON_AddNumberToObject(root, "reset_reason",   (double)d.reset_reason);
    cJSON_AddStringToObject(root, "reset_reason_str",
                            crash_diag_reset_reason_str(d.reset_reason));
    cJSON_AddNumberToObject(root, "last_uptime_sec", (double)d.last_uptime_sec);
    cJSON_AddNumberToObject(root, "min_free_heap",   (double)d.min_free_heap);
    send_json(req, 200, root);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/diag/reset                                               */
/* ================================================================== */

static esp_err_t handle_post_diag_reset(httpd_req_t *req)
{
    crash_diag_reset_boot_count();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddNumberToObject(resp, "boot_count", 0);
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/wifi                                                     */
/* ================================================================== */

static esp_err_t handle_post_wifi(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "No body or too large");
        send_json(req, 400, e); cJSON_Delete(e); return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "Invalid JSON");
        send_json(req, 400, e); cJSON_Delete(e); return ESP_OK;
    }

    cJSON *ssid_j     = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_j     = cJSON_GetObjectItem(root, "password");
    cJSON *hostname_j = cJSON_GetObjectItem(root, "hostname");

    if (!ssid_j || !cJSON_IsString(ssid_j) || ssid_j->valuestring[0] == '\0') {
        cJSON_Delete(root);
        cJSON *e = cJSON_CreateObject(); cJSON_AddStringToObject(e, "error", "ssid is required");
        send_json(req, 400, e); cJSON_Delete(e); return ESP_OK;
    }
    if (!hostname_j || !cJSON_IsString(hostname_j) || hostname_j->valuestring[0] == '\0') {
        cJSON_Delete(root);
        cJSON *e = cJSON_CreateObject(); cJSON_AddStringToObject(e, "error", "hostname is required");
        send_json(req, 400, e); cJSON_Delete(e); return ESP_OK;
    }

    char ssid_buf[33]     = {};
    char pass_buf[65]     = {};
    char hostname_buf[33] = {};
    strncpy(ssid_buf,     ssid_j->valuestring, sizeof(ssid_buf) - 1);
    strncpy(pass_buf,     (pass_j && cJSON_IsString(pass_j)) ? pass_j->valuestring : "",
                          sizeof(pass_buf) - 1);
    strncpy(hostname_buf, hostname_j->valuestring, sizeof(hostname_buf) - 1);
    cJSON_Delete(root);

    esp_err_t err = wifi_manager_set_credentials(ssid_buf, pass_buf);
    if (err == ESP_OK) err = wifi_manager_save_hostname(hostname_buf);

    cJSON *resp = cJSON_CreateObject();
    if (err == ESP_OK) {
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "hostname", hostname_buf);
        send_json(req, 200, resp);
    } else {
        cJSON_AddStringToObject(resp, "error", "Failed to save credentials");
        send_json(req, 500, resp);
    }
    cJSON_Delete(resp);
    if (err == ESP_OK) { vTaskDelay(pdMS_TO_TICKS(1000)); esp_restart(); }
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/wifi-reset                                               */
/* ================================================================== */

static esp_err_t handle_wifi_reset(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "resetting");
    send_json(req, 200, resp); cJSON_Delete(resp);
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_clear_credentials();
    esp_restart();
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/restart                                                  */
/* ================================================================== */

static esp_err_t handle_restart(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "restarting");
    send_json(req, 200, resp); cJSON_Delete(resp);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/zb-reset                                                 */
/* ================================================================== */

static esp_err_t handle_zb_reset(httpd_req_t *req)
{
    extern void zigbee_factory_reset(void);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "resetting");
    send_json(req, 200, resp); cJSON_Delete(resp);
    vTaskDelay(pdMS_TO_TICKS(500));
    zigbee_factory_reset();
    return ESP_OK;
}

/* ================================================================== */
/*  POST /api/factory-reset                                            */
/* ================================================================== */

static esp_err_t handle_factory_reset(httpd_req_t *req)
{
    extern void zigbee_full_factory_reset(void);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "resetting");
    send_json(req, 200, resp); cJSON_Delete(resp);
    vTaskDelay(pdMS_TO_TICKS(500));
    zigbee_full_factory_reset();
    return ESP_OK;
}

/* ================================================================== */
/*  OTA endpoints                                                      */
/* ================================================================== */

static esp_err_t handle_post_ota(httpd_req_t *req)
{
    char *body = read_body(req);
    const char *url = ota_check_get_index_url();
    cJSON *root = NULL;

    if (body) {
        root = cJSON_Parse(body);
        if (root) {
            cJSON *url_j = cJSON_GetObjectItemCaseSensitive(root, "url");
            if (cJSON_IsString(url_j) && url_j->valuestring[0] != '\0') {
                url = url_j->valuestring;
            }
        }
    }

    esp_err_t ret = zigbee_ota_start_wifi_update(url);
    cJSON_Delete(root);
    free(body);

    if (ret == ESP_OK) {
        httpd_resp_set_status(req, "202 Accepted");
        web_server_base_sse_notify("ota");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
    }

    cJSON *resp = cJSON_CreateObject();
    if (ret == ESP_OK) {
        cJSON_AddStringToObject(resp, "status", "update_started");
    } else {
        cJSON_AddStringToObject(resp, "error",
            ret == ESP_ERR_INVALID_STATE ? "OTA already in progress" : "Failed to start OTA");
    }
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t handle_ota_upload(httpd_req_t *req)
{
#ifdef CONFIG_IDF_TARGET_ESP32C6
    if (req->content_len == 0) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "no file");
        send_json(req, 400, e); cJSON_Delete(e); return ESP_OK;
    }
    esp_err_t ret = ota_upload_transport_flash(req);
    cJSON *resp = cJSON_CreateObject();
    if (ret == ESP_OK) {
        cJSON_AddStringToObject(resp, "status", "ok");
        send_json(req, 200, resp);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "error", "OTA already in progress");
        send_json(req, 409, resp);
    } else {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "error", "flash failed");
        send_json(req, 500, resp);
    }
    cJSON_Delete(resp);
#else
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "error", "not supported on this target");
    send_json(req, 501, e); cJSON_Delete(e);
#endif
    return ESP_OK;
}

static esp_err_t handle_get_ota_status(httpd_req_t *req)
{
    /* firmware_version is "v2.3.2" — strip leading 'v' for plain comparison */
    const char *plain = s_cfg ? s_cfg->firmware_version : "";
    if (plain[0] == 'v') plain++;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "available",   ota_check_available());
    cJSON_AddStringToObject(resp, "current",   plain);
    cJSON_AddStringToObject(resp, "latest",    ota_check_latest_version());
    cJSON_AddBoolToObject(resp, "in_progress", zigbee_ota_is_in_progress());
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t handle_post_ota_check(httpd_req_t *req)
{
    ota_check_trigger();

    const char *plain = s_cfg ? s_cfg->firmware_version : "";
    if (plain[0] == 'v') plain++;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "available",   ota_check_available());
    cJSON_AddStringToObject(resp, "current",   plain);
    cJSON_AddStringToObject(resp, "latest",    ota_check_latest_version());
    cJSON_AddBoolToObject(resp, "in_progress", zigbee_ota_is_in_progress());
    send_json(req, 200, resp);
    cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t handle_get_ota_interval(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "interval_hours", ota_check_get_interval_hours());
    send_json(req, 200, resp); cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t handle_post_ota_interval(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) { send_json(req, 400, NULL); return ESP_OK; }
    cJSON *root = cJSON_Parse(body); free(body);
    if (!root) { send_json(req, 400, NULL); return ESP_OK; }
    cJSON *h = cJSON_GetObjectItem(root, "interval_hours");
    if (!cJSON_IsNumber(h)) { cJSON_Delete(root); send_json(req, 400, NULL); return ESP_OK; }
    ota_check_set_interval_hours((uint16_t)h->valueint);
    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    send_json(req, 200, resp); cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t handle_get_ota_index_url(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "url", ota_check_get_index_url());
    send_json(req, 200, resp); cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t handle_post_ota_index_url(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) { send_json(req, 400, NULL); return ESP_OK; }
    cJSON *root = cJSON_Parse(body); free(body);
    if (!root) { send_json(req, 400, NULL); return ESP_OK; }
    cJSON *u = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(u)) { cJSON_Delete(root); send_json(req, 400, NULL); return ESP_OK; }
    ota_check_set_index_url(u->valuestring);
    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "url", ota_check_get_index_url());
    send_json(req, 200, resp); cJSON_Delete(resp);
    return ESP_OK;
}

/* ================================================================== */
/*  404 handler                                                        */
/* ================================================================== */

static esp_err_t handle_not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    send_redirect(req, "http://192.168.4.1/");
    return ESP_OK;
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

esp_err_t web_server_base_start(const web_server_base_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg_copy = *cfg;
    s_cfg = &s_cfg_copy;

    /* Build parameterised setup page */
    s_setup_html = malloc(4608);
    if (s_setup_html) {
        snprintf(s_setup_html, 4608, SETUP_HTML_TMPL,
                 cfg->device_name,      /* title */
                 cfg->device_name,      /* h1    */
                 cfg->firmware_version);/* badge */
    }

    mount_spiffs();
    sync_web_assets();

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.lru_purge_enable  = true;
    hcfg.stack_size        = 8192;
    hcfg.max_uri_handlers  = 40;  /* base uses ~23; leaves room for device-specific */

    esp_err_t err = httpd_start(&s_server, &hcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        free(s_setup_html); s_setup_html = NULL;
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",                    .method = HTTP_GET,  .handler = handle_root            },
        { .uri = "/app.js",              .method = HTTP_GET,  .handler = handle_app_js          },
        { .uri = "/style.css",           .method = HTTP_GET,  .handler = handle_style_css       },
        { .uri = "/wifi.js",             .method = HTTP_GET,  .handler = handle_wifi_js         },
        { .uri = "/generate_204",        .method = HTTP_GET,  .handler = handle_generate_204    },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET,  .handler = handle_hotspot_detect  },
        { .uri = "/ncsi.txt",            .method = HTTP_GET,  .handler = handle_ncsi            },
        { .uri = "/api/wifi-scan",       .method = HTTP_GET,  .handler = handle_wifi_scan       },
        { .uri = "/api/status",          .method = HTTP_GET,  .handler = handle_get_status      },
        { .uri = "/api/diag",            .method = HTTP_GET,  .handler = handle_get_diag        },
        { .uri = "/api/diag/reset",      .method = HTTP_POST, .handler = handle_post_diag_reset },
        { .uri = "/api/wifi",            .method = HTTP_POST, .handler = handle_post_wifi       },
        { .uri = "/api/wifi-reset",      .method = HTTP_POST, .handler = handle_wifi_reset      },
        { .uri = "/api/restart",         .method = HTTP_POST, .handler = handle_restart         },
        { .uri = "/api/zb-reset",        .method = HTTP_POST, .handler = handle_zb_reset        },
        { .uri = "/api/factory-reset",   .method = HTTP_POST, .handler = handle_factory_reset   },
        { .uri = "/api/ota",             .method = HTTP_POST, .handler = handle_post_ota        },
        { .uri = "/api/ota/upload",      .method = HTTP_POST, .handler = handle_ota_upload      },
        { .uri = "/api/ota/status",      .method = HTTP_GET,  .handler = handle_get_ota_status  },
        { .uri = "/api/ota/check",       .method = HTTP_POST, .handler = handle_post_ota_check  },
        { .uri = "/api/ota/interval",    .method = HTTP_GET,  .handler = handle_get_ota_interval  },
        { .uri = "/api/ota/interval",    .method = HTTP_POST, .handler = handle_post_ota_interval },
        { .uri = "/api/ota/index-url",   .method = HTTP_GET,  .handler = handle_get_ota_index_url },
        { .uri = "/api/ota/index-url",   .method = HTTP_POST, .handler = handle_post_ota_index_url},
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handle_not_found);

    /* Start background OTA check (first check fires 15 s after init) */
    ota_check_config_t ota_cfg = {
        .image_type      = cfg->ota_image_type,
        .current_version = cfg->current_version_hex,
        .nvs_namespace   = cfg->nvs_namespace,
    };
    ota_check_init(&ota_cfg);

    ESP_LOGI(TAG, "HTTP server started (%s %s)", cfg->device_name, cfg->firmware_version);
    return ESP_OK;
}

esp_err_t web_server_base_register(const char *uri, httpd_method_t method,
                                   esp_err_t (*handler)(httpd_req_t *req),
                                   bool is_websocket)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;
    httpd_uri_t h = {
        .uri          = uri,
        .method       = method,
        .handler      = handler,
        .is_websocket = is_websocket,
    };
    return httpd_register_uri_handler(s_server, &h);
}

void web_server_base_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    free(s_setup_html);
    s_setup_html = NULL;
    s_cfg = NULL;
}

httpd_handle_t web_server_base_get_server(void) { return s_server; }
