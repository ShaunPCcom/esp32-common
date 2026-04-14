/**
 * @file zigbee_signal_handler.c
 * @brief Shared Zigbee network lifecycle signal handler.
 *
 * Implements esp_zb_app_signal_handler() and all common network lifecycle
 * behaviour. Projects register callbacks for device-specific actions via
 * zigbee_signal_handler_register().
 */

#include "zigbee_signal_handler.h"

#include "esp_log.h"

/* C wrappers for BoardLed (defined in board_led component) */
extern void board_led_set_state_off(void);
extern void board_led_set_state_not_joined(void);
extern void board_led_set_state_pairing(void);
extern void board_led_set_state_joined(void);
extern void board_led_set_state_error(void);
#include "esp_system.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "zb_handler";

bool s_network_joined = false;

static const zigbee_signal_hooks_t *s_hooks = NULL;

/* ================================================================== */
/*  Internal callbacks                                                 */
/* ================================================================== */

static void steering_retry_cb(uint8_t param)
{
    ESP_LOGI(TAG, "Retrying network steering...");
    board_led_set_state_pairing();
    esp_zb_bdb_start_top_level_commissioning(param);
}

void reboot_cb(uint8_t param)
{
    (void)param;
    esp_restart();
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

void zigbee_signal_handler_register(const zigbee_signal_hooks_t *hooks)
{
    s_hooks = hooks;
}

bool zigbee_is_network_joined(void)
{
    return s_network_joined;
}

void zigbee_factory_reset(void)
{
    ESP_LOGW(TAG, "Zigbee network reset — leaving network, keeping config");
    board_led_set_state_error();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_zb_factory_reset();
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void zigbee_full_factory_reset(void)
{
    ESP_LOGW(TAG, "FULL factory reset — erasing Zigbee network + NVS config");
    board_led_set_state_error();
    vTaskDelay(pdMS_TO_TICKS(200));

    const char *ns = (s_hooks && s_hooks->nvs_namespace) ? s_hooks->nvs_namespace : NULL;
    if (ns) {
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
            ESP_LOGI(TAG, "NVS namespace '%s' erased", ns);
        }
    } else {
        ESP_LOGW(TAG, "Full factory reset: no NVS namespace registered, skipping erase");
    }

    esp_zb_factory_reset();
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/* ================================================================== */
/*  Signal handler                                                     */
/* ================================================================== */

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct ? signal_struct->p_app_signal : NULL;
    esp_zb_app_signal_type_t sig = p_sg_p ? *p_sg_p : 0;
    esp_err_t status = signal_struct ? signal_struct->esp_err_status : ESP_OK;

    switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Stack initialized, starting network steering");
        board_led_set_state_pairing();
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_NETWORK_STEERING);
        if (s_hooks && s_hooks->on_stack_init) {
            s_hooks->on_stack_init();
        }
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Factory new device, starting network steering");
                board_led_set_state_pairing();
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted, already joined network");
                board_led_set_state_joined();
                s_network_joined = true;
                if (s_hooks && s_hooks->on_joined) {
                    s_hooks->on_joined();
                }
#if CONFIG_IDF_TARGET_ESP32C6
                /* C6 runs as End Device — unlike a Router it sends no routing
                 * announcements on boot, so Z2M won't detect the device is back
                 * without an explicit ZDO Device_annce broadcast. */
                esp_zb_zdo_device_announcement_req();
#endif
            }
        } else {
            ESP_LOGE(TAG, "Device start/reboot failed: %s", esp_err_to_name(status));
            board_led_set_state_error();
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (status == ESP_OK) {
            ESP_LOGI(TAG, "Successfully joined Zigbee network!");
            board_led_set_state_joined();
            s_network_joined = true;
            if (s_hooks && s_hooks->on_joined) {
                s_hooks->on_joined();
            }
        } else {
            ESP_LOGW(TAG, "Network steering failed (%s), retrying in 5s...",
                     esp_err_to_name(status));
            board_led_set_state_error();
            esp_zb_scheduler_alarm(steering_retry_cb, ESP_ZB_BDB_NETWORK_STEERING, 5000);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        ESP_LOGW(TAG, "Left Zigbee network");
        board_led_set_state_not_joined();
        s_network_joined = false;
        if (s_hooks && s_hooks->on_left) {
            s_hooks->on_left();
        }
        esp_zb_scheduler_alarm(steering_retry_cb, ESP_ZB_BDB_NETWORK_STEERING, 1000);
        break;

    case ESP_ZB_COMMON_SIGNAL_CAN_SLEEP:
        break;

    default:
        if (s_hooks && s_hooks->on_unhandled_signal) {
            s_hooks->on_unhandled_signal(signal_struct);
        } else {
            ESP_LOGI(TAG, "Zigbee signal: 0x%x, status: %s", sig, esp_err_to_name(status));
        }
        break;
    }
}
