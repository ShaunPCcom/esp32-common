/**
 * @file board_led.cpp
 * @brief Implementation of BoardLed class for WS2812B status indication
 *
 * Uses the ESP-IDF 5.x RMT TX API with a bytes encoder.
 * WS2812B: GRB byte order, 24-bit per pixel.
 * RMT resolution 10 MHz (100 ns/tick). Timing:
 *   bit0: 400 ns high, 800 ns low
 *   bit1: 800 ns high, 400 ns low
 *   reset: idle low >50 µs (satisfied by inter-timer gap)
 */

#include "board_led.hpp"
#include "esp_log.h"

static const char* TAG = "BoardLed";

// Timing constants
static constexpr uint32_t TIMED_STATE_US      = 5 * 1000 * 1000;  // 5 seconds
static constexpr uint32_t RMT_RESOLUTION_HZ   = 10000000;          // 10 MHz, 100 ns/tick

// Blink intervals (microseconds)
static constexpr uint32_t BLINK_NOT_JOINED_US = 250 * 1000;  // ~2 Hz
static constexpr uint32_t BLINK_PAIRING_US    = 250 * 1000;  // ~2 Hz
static constexpr uint32_t BLINK_ERROR_US      = 100 * 1000;  // ~5 Hz

// Color definitions (R, G, B values 0-255)
static constexpr uint8_t COLOR_AMBER_R  = 40;
static constexpr uint8_t COLOR_AMBER_G  = 20;
static constexpr uint8_t COLOR_AMBER_B  = 0;

static constexpr uint8_t COLOR_BLUE_R   = 0;
static constexpr uint8_t COLOR_BLUE_G   = 0;
static constexpr uint8_t COLOR_BLUE_B   = 40;

static constexpr uint8_t COLOR_GREEN_R  = 0;
static constexpr uint8_t COLOR_GREEN_G  = 60;
static constexpr uint8_t COLOR_GREEN_B  = 0;

static constexpr uint8_t COLOR_RED_R    = 60;
static constexpr uint8_t COLOR_RED_G    = 0;
static constexpr uint8_t COLOR_RED_B    = 0;

BoardLed::BoardLed(uint8_t gpio)
    : m_rmt_chan(nullptr)
    , m_bytes_enc(nullptr)
    , m_blink_timer(nullptr)
    , m_timeout_timer(nullptr)
    , m_state(State::OFF)
    , m_blink_on(false)
{
    // Create RMT TX channel for WS2812B
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num        = static_cast<gpio_num_t>(gpio),
        .clk_src         = RMT_CLK_SRC_DEFAULT,
        .resolution_hz   = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
        .intr_priority   = 0,
        .flags = {
            .invert_out = false,
            .with_dma = false,
            .allow_pd = false,
            .init_level = 0,
        },
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &m_rmt_chan));

    // Bytes encoder: WS2812B timing at 10 MHz
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .duration0 = 4, .level0 = 1,   // 400 ns high
                  .duration1 = 8, .level1 = 0 },  // 800 ns low
        .bit1 = { .duration0 = 8, .level0 = 1,   // 800 ns high
                  .duration1 = 4, .level1 = 0 },  // 400 ns low
        .flags = { .msb_first = 1 },
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &m_bytes_enc));
    ESP_ERROR_CHECK(rmt_enable(m_rmt_chan));

    // Create blink timer
    const esp_timer_create_args_t blink_args = {
        .callback = blink_timer_cb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_blink",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&blink_args, &m_blink_timer));

    // Create timeout timer
    const esp_timer_create_args_t timeout_args = {
        .callback = timeout_timer_cb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_timeout",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timeout_args, &m_timeout_timer));

    ESP_LOGI(TAG, "Initialized on GPIO%d (RMT)", gpio);
}

BoardLed::~BoardLed()
{
    // Stop timers
    if (m_blink_timer) {
        esp_timer_stop(m_blink_timer);
        esp_timer_delete(m_blink_timer);
    }
    if (m_timeout_timer) {
        esp_timer_stop(m_timeout_timer);
        esp_timer_delete(m_timeout_timer);
    }

    // Cleanup RMT resources
    if (m_rmt_chan) {
        rmt_disable(m_rmt_chan);
        rmt_del_encoder(m_bytes_enc);
        rmt_del_channel(m_rmt_chan);
    }
}

void BoardLed::set_state(State state)
{
    m_state    = state;
    m_blink_on = false;

    // Stop any active timers
    esp_timer_stop(m_blink_timer);
    esp_timer_stop(m_timeout_timer);

    switch (state) {
    case State::OFF:
        clear();
        break;

    case State::NOT_JOINED:
        // Blinking amber ~2 Hz, indefinite
        esp_timer_start_periodic(m_blink_timer, BLINK_NOT_JOINED_US);
        break;

    case State::PAIRING:
        // Blinking blue ~2 Hz, indefinite
        esp_timer_start_periodic(m_blink_timer, BLINK_PAIRING_US);
        break;

    case State::JOINED:
        // Solid green for 5s, then OFF
        apply_color(COLOR_GREEN_R, COLOR_GREEN_G, COLOR_GREEN_B);
        esp_timer_start_once(m_timeout_timer, TIMED_STATE_US);
        break;

    case State::ERROR:
        // Blinking red ~5 Hz for 5s, then PAIRING
        esp_timer_start_periodic(m_blink_timer, BLINK_ERROR_US);
        esp_timer_start_once(m_timeout_timer, TIMED_STATE_US);
        break;

    default:
        clear();
        break;
    }
}

void BoardLed::apply_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!m_rmt_chan) return;

    // WS2812B: GRB byte order
    uint8_t grb[3] = {g, r, b};
    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags = {
            .eot_level = 0,
            .queue_nonblocking = false,
        },
    };
    rmt_transmit(m_rmt_chan, m_bytes_enc, grb, sizeof(grb), &tx_cfg);
}

void BoardLed::clear()
{
    apply_color(0, 0, 0);
}

void BoardLed::blink_timer_cb(void* arg)
{
    BoardLed* self = static_cast<BoardLed*>(arg);
    self->on_blink();
}

void BoardLed::timeout_timer_cb(void* arg)
{
    BoardLed* self = static_cast<BoardLed*>(arg);
    self->on_timeout();
}

void BoardLed::on_blink()
{
    m_blink_on = !m_blink_on;

    switch (m_state) {
    case State::NOT_JOINED:
        if (m_blink_on) {
            apply_color(COLOR_AMBER_R, COLOR_AMBER_G, COLOR_AMBER_B);
        } else {
            clear();
        }
        break;

    case State::PAIRING:
        if (m_blink_on) {
            apply_color(COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
        } else {
            clear();
        }
        break;

    case State::ERROR:
        if (m_blink_on) {
            apply_color(COLOR_RED_R, COLOR_RED_G, COLOR_RED_B);
        } else {
            clear();
        }
        break;

    default:
        break;
    }
}

void BoardLed::on_timeout()
{
    switch (m_state) {
    case State::JOINED:
        set_state(State::OFF);
        break;

    case State::ERROR:
        set_state(State::PAIRING);
        break;

    default:
        break;
    }
}
