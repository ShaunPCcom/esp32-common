#include "zigbee_button.hpp"
#include "driver/gpio.h"
#include "esp_log.h"

static const char* TAG = "ButtonHandler";

ButtonHandler::ButtonHandler(uint8_t gpio, uint32_t network_reset_ms, uint32_t full_reset_ms)
    : m_gpio(gpio),
      m_network_reset_ms(network_reset_ms),
      m_full_reset_ms(full_reset_ms),
      m_task_handle(nullptr),
      m_network_reset_cb(nullptr),
      m_full_reset_cb(nullptr),
      m_led_cb(nullptr)
{
    // Configure GPIO as input with pull-up
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << m_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
#ifdef CONFIG_IDF_TARGET_ESP32H2
        .hys_ctrl_mode = GPIO_HYS_SOFT_DISABLE,
#endif
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "ButtonHandler created (GPIO %d, network_reset=%lums, full_reset=%lums)",
             m_gpio, m_network_reset_ms, m_full_reset_ms);
}

ButtonHandler::~ButtonHandler()
{
    stop();
}

void ButtonHandler::start()
{
    if (m_task_handle != nullptr) {
        ESP_LOGW(TAG, "Task already running, ignoring start()");
        return;
    }

    xTaskCreate(task_func, "btn_task", 2048, this, 5, &m_task_handle);
    ESP_LOGI(TAG, "Button task started");
}

void ButtonHandler::stop()
{
    if (m_task_handle != nullptr) {
        vTaskDelete(m_task_handle);
        m_task_handle = nullptr;
        ESP_LOGI(TAG, "Button task stopped");
    }
}

void ButtonHandler::set_network_reset_callback(Callback cb)
{
    m_network_reset_cb = cb;
}

void ButtonHandler::set_full_reset_callback(Callback cb)
{
    m_full_reset_cb = cb;
}

void ButtonHandler::set_led_callback(void(*cb)(int state))
{
    m_led_cb = cb;
}

void ButtonHandler::task_func(void* arg)
{
    ButtonHandler* handler = static_cast<ButtonHandler*>(arg);
    handler->run();
}

void ButtonHandler::run()
{
    uint32_t held_ms = 0;
    uint32_t blink_counter = 0;

    while (1) {
        if (gpio_get_level(static_cast<gpio_num_t>(m_gpio)) == 0) {
            // Button pressed (active low)
            held_ms += 100;
            blink_counter++;

            // LED feedback during hold (if callback provided)
            if (m_led_cb != nullptr) {
                if (held_ms >= 1000 && held_ms < m_network_reset_ms) {
                    // 1s-3s: amber/not-joined state (fast blink)
                    m_led_cb((blink_counter % 2) ? 1 : 2);
                } else if (held_ms >= m_network_reset_ms && held_ms < m_full_reset_ms) {
                    // 3s-10s: slower alternating blink (network reset pending)
                    m_led_cb(((blink_counter / 5) % 2) ? 1 : 2);
                } else if (held_ms >= m_full_reset_ms) {
                    // 10s+: solid red (full reset pending)
                    m_led_cb(2);
                }
            }
        } else {
            // Button released
            if (held_ms >= m_full_reset_ms) {
                // Held for 10s+ → full factory reset
                if (m_full_reset_cb != nullptr) {
                    ESP_LOGI(TAG, "Button held %lums, triggering full factory reset", held_ms);
                    m_full_reset_cb();
                } else {
                    ESP_LOGW(TAG, "Full reset callback not set");
                }
            } else if (held_ms >= m_network_reset_ms) {
                // Held for 3s+ → network reset
                if (m_network_reset_cb != nullptr) {
                    ESP_LOGI(TAG, "Button held %lums, triggering network reset", held_ms);
                    m_network_reset_cb();
                } else {
                    ESP_LOGW(TAG, "Network reset callback not set");
                }
            } else if (held_ms >= 1000) {
                // Held for 1s+ but released before threshold → restore LED state
                if (m_led_cb != nullptr) {
                    m_led_cb(0);  // State 0 = restore previous LED state
                }
            }

            // Reset counters
            held_ms = 0;
            blink_counter = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
