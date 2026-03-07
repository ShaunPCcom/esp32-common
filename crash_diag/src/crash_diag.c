// SPDX-License-Identifier: MIT
#include "crash_diag.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "crash_diag";

/* NVS namespace for crash diagnostics (separate from main app config) */
#define CRASH_DIAG_NVS_NAMESPACE "crash_diag"
#define CRASH_DIAG_NVS_BOOT_COUNT "boot_count"

/**
 * RTC memory structure - survives all resets (software, panic, WDT, brownout)
 * but NOT power loss. Marked RTC_NOINIT_ATTR so startup code does not zero it
 * on reset. Magic number detects invalid/uninitialized state on first power-on.
 *
 * NOTE: RTC_NOINIT_ATTR maps to the .rtc_noinit NOLOAD section on ESP32-H2
 * (unified LP RAM). This is preserved across software/WDT/panic/brownout
 * resets but cleared on power loss. Use RTC_NOINIT_ATTR, NOT RTC_DATA_ATTR —
 * the latter is zeroed on every reset except deep sleep.
 */
typedef struct {
    uint32_t magic;             /**< Magic number to detect valid data (0xD1A65BAD) */
    uint8_t  reset_reason;      /**< Reset reason from esp_reset_reason() */
    uint32_t uptime_sec;        /**< Uptime in seconds at last update */
    uint32_t boot_count_copy;   /**< Copy of boot count for cross-check */
} rtc_diag_data_t;

#define RTC_DIAG_MAGIC 0xD1A65BAD

static RTC_NOINIT_ATTR rtc_diag_data_t rtc_data;
static crash_diag_data_t s_current_diag;

/* ================================================================== */
/*  Internal helpers                                                   */
/* ================================================================== */

static esp_err_t load_and_increment_boot_count(uint32_t *out_count)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CRASH_DIAG_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t boot_count = 0;
    err = nvs_get_u32(nvs, CRASH_DIAG_NVS_BOOT_COUNT, &boot_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        boot_count = 0;
        ESP_LOGI(TAG, "First boot, initializing boot_count");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read boot_count: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return err;
    }

    boot_count++;
    err = nvs_set_u32(nvs, CRASH_DIAG_NVS_BOOT_COUNT, boot_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write boot_count: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return err;
    }

    err = nvs_commit(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit boot_count: %s", esp_err_to_name(err));
    }

    nvs_close(nvs);
    *out_count = boot_count;
    return ESP_OK;
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

esp_err_t crash_diag_init(void)
{
    memset(&s_current_diag, 0, sizeof(s_current_diag));

    esp_reset_reason_t reset_reason = esp_reset_reason();

    esp_err_t err = load_and_increment_boot_count(&s_current_diag.boot_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Boot count unavailable, continuing with 0");
        s_current_diag.boot_count = 0;
    }

    bool rtc_valid = (rtc_data.magic == RTC_DIAG_MAGIC);

    if (rtc_valid) {
        s_current_diag.reset_reason    = rtc_data.reset_reason;
        s_current_diag.last_uptime_sec = rtc_data.uptime_sec;
        ESP_LOGI(TAG, "Boot #%lu, Reset: %s, Last uptime: %lu sec",
                 s_current_diag.boot_count,
                 crash_diag_reset_reason_str(s_current_diag.reset_reason),
                 s_current_diag.last_uptime_sec);
    } else {
        s_current_diag.reset_reason    = (uint8_t)reset_reason;
        s_current_diag.last_uptime_sec = 0;
        ESP_LOGI(TAG, "Boot #%lu, Reset: %s (RTC invalid, likely power loss)",
                 s_current_diag.boot_count,
                 crash_diag_reset_reason_str(s_current_diag.reset_reason));
    }

    s_current_diag.min_free_heap = esp_get_minimum_free_heap_size();

    /* Prepare RTC memory for next boot */
    rtc_data.magic           = RTC_DIAG_MAGIC;
    rtc_data.reset_reason    = (uint8_t)reset_reason;
    rtc_data.uptime_sec      = 0;
    rtc_data.boot_count_copy = s_current_diag.boot_count;

    return ESP_OK;
}

esp_err_t crash_diag_get_data(crash_diag_data_t *data)
{
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }
    s_current_diag.min_free_heap = esp_get_minimum_free_heap_size();
    memcpy(data, &s_current_diag, sizeof(crash_diag_data_t));
    return ESP_OK;
}

const char *crash_diag_reset_reason_str(uint8_t reason)
{
    switch ((esp_reset_reason_t)reason) {
        case ESP_RST_UNKNOWN:    return "UNKNOWN";
        case ESP_RST_POWERON:    return "POWERON";
        case ESP_RST_EXT:        return "EXTERNAL";
        case ESP_RST_SW:         return "SOFTWARE";
        case ESP_RST_PANIC:      return "PANIC";
        case ESP_RST_INT_WDT:    return "INT_WDT";
        case ESP_RST_TASK_WDT:   return "TASK_WDT";
        case ESP_RST_WDT:        return "WDT";
        case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:   return "BROWNOUT";
        case ESP_RST_SDIO:       return "SDIO";
        case ESP_RST_USB:        return "USB";
        case ESP_RST_JTAG:       return "JTAG";
        case ESP_RST_EFUSE:      return "EFUSE";
        case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
        default:                 return "INVALID";
    }
}

void crash_diag_update_uptime(uint32_t uptime_sec)
{
    rtc_data.uptime_sec = uptime_sec;
}
