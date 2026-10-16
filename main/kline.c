#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "soc/gpio_sig_map.h"

#include "kline.h"

static const char *TAG = "kline";

#define KL_PORT      ((uart_port_t)CONFIG_RRP38_KLINE_UART_PORT)
#define KL_TX_GPIO   (CONFIG_RRP38_KLINE_TX_GPIO)
#define KL_RX_GPIO   (CONFIG_RRP38_KLINE_RX_GPIO)
#define KL_L_GPIO    (CONFIG_RRP38_LLINE_TX_GPIO)
#define KL_BAUD      (CONFIG_RRP38_KLINE_BAUD)
#define KL_RX_BUF    2048

/* IDF v6 relocated uart_periph_signal into the esp_hal_uart component and
 * dropped the SOC_UART_*_PIN_IDX macros, so the TX output signal index is
 * taken straight from soc/gpio_sig_map.h. */
#if CONFIG_RRP38_KLINE_UART_PORT == 1
#  define KL_TX_SIG  U1TXD_OUT_IDX
#elif CONFIG_RRP38_KLINE_UART_PORT == 2
#  define KL_TX_SIG  U2TXD_OUT_IDX
#else
#  error "K-line UART port must be 1 or 2 (UART0 is the debug console)"
#endif

static bool s_started  = false;
static bool s_tx_armed = false;

/* Bus ownership: one user of the UART at a time (console command or web
 * sniff). Created once and never deleted, so it outlives kline_stop(). */
static SemaphoreHandle_t s_bus_lock;
static const char *volatile s_bus_owner;

/* ------------------------------------------------------------------ */
/* Timing and bit-bang helpers                                         */
/*                                                                     */
/* All of this exists only to drive the init sequences, which are      */
/* transmit-only - so in a passive build none of it is compiled.       */
/* ------------------------------------------------------------------ */
#if CONFIG_RRP38_ALLOW_TX

/* Sleep until an absolute microsecond deadline. Uses vTaskDelay for the bulk
 * of the wait and busy-waits the last ~2ms, which keeps the 200ms bit cells
 * of a 5-baud init well inside tolerance without hogging the CPU. */
static void delay_until_us(int64_t deadline_us)
{
    for (;;) {
        int64_t remaining = deadline_us - esp_timer_get_time();
        if (remaining <= 0) {
            return;
        }
        if (remaining > 2500) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)((remaining - 2000) / 1000)));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pin routing: hand the TX pin between the UART and plain GPIO so the  */
/* 5-baud address can be bit-banged. 5 baud is far below the minimum    */
/* rate the UART clock divider can reach, so it must be done by hand.   */
/* ------------------------------------------------------------------ */

#if CONFIG_RRP38_INIT_DRIVER_GPIO < 0
static void tx_pin_to_gpio(void)
{
    /* Drive the output register high *before* taking the pin, so the K-line
     * never glitches low during the handover - a low glitch looks like a
     * start bit to every module on the bus. */
    gpio_set_level((gpio_num_t)KL_TX_GPIO, 1);
    gpio_set_direction((gpio_num_t)KL_TX_GPIO, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(KL_TX_GPIO, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_level((gpio_num_t)KL_TX_GPIO, 1);
}

static void tx_pin_to_uart(void)
{
    esp_rom_gpio_connect_out_signal(KL_TX_GPIO, KL_TX_SIG, false, false);
}
#endif /* CONFIG_RRP38_INIT_DRIVER_GPIO < 0 */

/* Drive one init bit. level is the *bus* logic level: 0 = dominant.
 *
 * With a bypass driver configured, the bit is driven through an external
 * MOSFET instead of the transceiver, because LIN parts implement a TxD
 * dominant timeout (NCV7329: 14/25/46 ms, TJA1021: 27/55/90 ms) that is far
 * shorter than the 200 ms bit cell of a 5-baud init and would release the
 * bus mid-bit.
 * The MOSFET is active-high, so it takes the inverse of the bus level. */
static inline void bitbang_set(int level)
{
#if CONFIG_RRP38_INIT_DRIVER_GPIO >= 0
    gpio_set_level((gpio_num_t)CONFIG_RRP38_INIT_DRIVER_GPIO, level ? 0 : 1);
#else
    gpio_set_level((gpio_num_t)KL_TX_GPIO, level);
#endif
#if CONFIG_RRP38_LLINE_TX_GPIO >= 0
    gpio_set_level((gpio_num_t)KL_L_GPIO, level);
#endif
}

/* Claim the bus for bit-banging. Without a bypass driver this hands the TX
 * pin from the UART to plain GPIO; with one, the UART keeps the pin and is
 * simply held recessive so the two drivers never fight. */
static void bitbang_begin(void)
{
#if CONFIG_RRP38_INIT_DRIVER_GPIO >= 0
    gpio_set_level((gpio_num_t)CONFIG_RRP38_INIT_DRIVER_GPIO, 0);
#else
    tx_pin_to_gpio();
#endif
}

static void bitbang_end(void)
{
#if CONFIG_RRP38_INIT_DRIVER_GPIO >= 0
    gpio_set_level((gpio_num_t)CONFIG_RRP38_INIT_DRIVER_GPIO, 0);
#else
    tx_pin_to_uart();
#endif
}

#endif /* CONFIG_RRP38_ALLOW_TX */

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

esp_err_t kline_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    if (!s_bus_lock) {
        /* Binary, not a mutex: a claim may be released by a different task
         * from the one that took it (a web sniff ends in its own task). */
        s_bus_lock = xSemaphoreCreateBinary();
        if (!s_bus_lock) {
            return ESP_ERR_NO_MEM;
        }
        xSemaphoreGive(s_bus_lock);
    }

    const uart_config_t cfg = {
        .baud_rate  = KL_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(KL_PORT, KL_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(KL_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(KL_PORT, KL_TX_GPIO, KL_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

#if CONFIG_RRP38_LLINE_TX_GPIO >= 0
    gpio_config_t lcfg = {
        .pin_bit_mask = 1ULL << KL_L_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&lcfg));
    gpio_set_level((gpio_num_t)KL_L_GPIO, 1);
#endif

#if CONFIG_RRP38_EN_GPIO >= 0
    /* Transceivers such as the NCV7329 (EN) and TJA1021 (SLP_N) idle in
     * sleep mode with an internal pull-down on that pin, and look completely
     * dead until it is driven high. Raised after uart_set_pin() on purpose:
     * the TJA1021 leaves its transmitter off if TXD is low when it enters
     * normal mode. */
    gpio_config_t encfg = {
        .pin_bit_mask = 1ULL << CONFIG_RRP38_EN_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&encfg));
    gpio_set_level((gpio_num_t)CONFIG_RRP38_EN_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(2));       /* tINIT_NORM before the TX is usable */
#endif

#if CONFIG_RRP38_INIT_DRIVER_GPIO >= 0
    /* Bypass MOSFET for the init sequences. Off (bus recessive) at boot. */
    gpio_config_t dcfg = {
        .pin_bit_mask = 1ULL << CONFIG_RRP38_INIT_DRIVER_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&dcfg));
    gpio_set_level((gpio_num_t)CONFIG_RRP38_INIT_DRIVER_GPIO, 0);
#endif

    s_started = true;
    ESP_LOGI(TAG, "K-line up: UART%d TX=%d RX=%d @ %d baud",
             CONFIG_RRP38_KLINE_UART_PORT, KL_TX_GPIO, KL_RX_GPIO, KL_BAUD);
#if CONFIG_RRP38_EN_GPIO >= 0
    ESP_LOGI(TAG, "transceiver EN on GPIO%d (driven high)", CONFIG_RRP38_EN_GPIO);
#endif
#if CONFIG_RRP38_INIT_DRIVER_GPIO >= 0
    ESP_LOGI(TAG, "init bypass driver on GPIO%d (TxD timeout workaround)",
             CONFIG_RRP38_INIT_DRIVER_GPIO);
#endif
#if !CONFIG_RRP38_ALLOW_TX
    ESP_LOGW(TAG, "built PASSIVE - transmit disabled at compile time");
#endif
    return ESP_OK;
}

void kline_stop(void)
{
    if (!s_started) {
        return;
    }
    kline_release_bus();
    uart_driver_delete(KL_PORT);
    s_started  = false;
}

void kline_release_bus(void)
{
    /* Disarm first, then let go of the bus: bypass MOSFET off and the
     * transceiver back to sleep. Leaves the UART alone, so it is safe to
     * call from another task while a console command is running. */
    s_tx_armed = false;
#if CONFIG_RRP38_INIT_DRIVER_GPIO >= 0
    gpio_set_level((gpio_num_t)CONFIG_RRP38_INIT_DRIVER_GPIO, 0);
#endif
#if CONFIG_RRP38_EN_GPIO >= 0
    gpio_set_level((gpio_num_t)CONFIG_RRP38_EN_GPIO, 0);
#endif
}

bool kline_started(void)
{
    return s_started;
}

bool kline_claim(const char *who)
{
    if (!s_bus_lock || xSemaphoreTake(s_bus_lock, 0) != pdTRUE) {
        return false;
    }
    s_bus_owner = who;
    return true;
}

void kline_unclaim(void)
{
    s_bus_owner = NULL;
    xSemaphoreGive(s_bus_lock);
}

const char *kline_owner(void)
{
    const char *o = s_bus_owner;
    return o ? o : "";
}

bool kline_tx_armed(void)
{
    return s_tx_armed;
}

void kline_set_tx_armed(bool armed)
{
    s_tx_armed = armed;
}

bool kline_tx_possible(void)
{
#if CONFIG_RRP38_ALLOW_TX
    return s_started && s_tx_armed;
#else
    return false;
#endif
}

void kline_flush_rx(void)
{
    if (s_started) {
        uart_flush_input(KL_PORT);
    }
}

uint8_t kline_checksum(const uint8_t *buf, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += buf[i];
    }
    return (uint8_t)(sum & 0xFF);
}

/* ------------------------------------------------------------------ */
/* Read / write                                                        */
/* ------------------------------------------------------------------ */

int kline_read(uint8_t *buf, size_t max,
               uint32_t first_timeout_ms, uint32_t gap_timeout_ms)
{
    if (!s_started || max == 0) {
        return -1;
    }

    size_t   n       = 0;
    uint32_t timeout = first_timeout_ms;

    while (n < max) {
        int r = uart_read_bytes(KL_PORT, buf + n, 1, pdMS_TO_TICKS(timeout));
        if (r != 1) {
            break;              /* timed out - end of frame */
        }
        n++;
        timeout = gap_timeout_ms;
    }
    return (int)n;
}

esp_err_t kline_write(const uint8_t *buf, size_t len)
{
#if !CONFIG_RRP38_ALLOW_TX
    (void)buf;
    (void)len;
    ESP_LOGE(TAG, "transmit disabled at compile time (CONFIG_RRP38_ALLOW_TX)");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!kline_tx_possible()) {
        ESP_LOGE(TAG, "transmit not armed - run 'arm --yes' first");
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < len; i++) {
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(KLINE_P4_MS));
        }

        if (uart_write_bytes(KL_PORT, &buf[i], 1) != 1) {
            return ESP_FAIL;
        }

        /* Half-duplex: our own byte comes straight back on RX. Consume it,
         * and use it as a free wiring self-test. */
        uint8_t echo = 0;
        int r = uart_read_bytes(KL_PORT, &echo, 1, pdMS_TO_TICKS(200));
        if (r != 1) {
            ESP_LOGE(TAG, "no echo for byte %u (0x%02X) - check wiring / Vbatt",
                     (unsigned)i, buf[i]);
            return ESP_ERR_TIMEOUT;
        }
        if (echo != buf[i]) {
            ESP_LOGE(TAG, "echo mismatch at byte %u: sent 0x%02X got 0x%02X",
                     (unsigned)i, buf[i], echo);
#if CONFIG_RRP38_ECHO_STRICT
            return ESP_ERR_INVALID_RESPONSE;
#endif
        }
    }
    return ESP_OK;
#endif
}

/* ------------------------------------------------------------------ */
/* Initialisation sequences                                            */
/* ------------------------------------------------------------------ */

esp_err_t kline_slow_init(uint8_t addr, kline_init_result_t *out)
{
#if !CONFIG_RRP38_ALLOW_TX
    (void)addr;
    (void)out;
    ESP_LOGE(TAG, "slow init needs transmit, disabled at compile time");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!kline_tx_possible()) {
        ESP_LOGE(TAG, "transmit not armed - run 'arm --yes' first");
        return ESP_ERR_INVALID_STATE;
    }

    kline_flush_rx();

    /* W5: the bus must be idle for >=300ms before an address is sent. */
    vTaskDelay(pdMS_TO_TICKS(KLINE_W5_MS));

    /* Bit-bang the address at 5 baud: 200ms per bit,
     * start(0) + 8 data bits LSB first + stop(1). */
    bitbang_begin();

    int64_t t = esp_timer_get_time();

    bitbang_set(0);                       /* start bit */
    t += 200000;
    delay_until_us(t);

    for (int b = 0; b < 8; b++) {
        bitbang_set((addr >> b) & 1);     /* LSB first */
        t += 200000;
        delay_until_us(t);
    }

    bitbang_set(1);                       /* stop bit */
    t += 200000;
    delay_until_us(t);

    int64_t addr_done_us = esp_timer_get_time();
    bitbang_end();

    /* The transceiver echoed our slow bits back into the UART as garbage. */
    kline_flush_rx();

    /* W1: ECU answers with 0x55 sync, 60..300ms after the address. */
    uint8_t sync = 0;
    if (uart_read_bytes(KL_PORT, &sync, 1,
                        pdMS_TO_TICKS(KLINE_W1_MAX_MS)) != 1) {
        ESP_LOGW(TAG, "addr 0x%02X: no sync byte (no such module, or bus idle)",
                 addr);
        return ESP_ERR_TIMEOUT;
    }

    if (out) {
        out->responded    = true;
        out->sync         = sync;
        out->w1_actual_ms =
            (uint32_t)((esp_timer_get_time() - addr_done_us) / 1000);
    }

    if (sync != 0x55) {
        ESP_LOGW(TAG, "addr 0x%02X: expected sync 0x55, got 0x%02X "
                      "(baud mismatch or a non-ISO9141 module)", addr, sync);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* W2 / W3: the two keyword bytes. */
    uint8_t kw1 = 0, kw2 = 0;
    if (uart_read_bytes(KL_PORT, &kw1, 1, pdMS_TO_TICKS(KLINE_W2_MAX_MS)) != 1 ||
        uart_read_bytes(KL_PORT, &kw2, 1, pdMS_TO_TICKS(KLINE_W3_MAX_MS)) != 1) {
        ESP_LOGW(TAG, "addr 0x%02X: sync seen but keyword bytes missing", addr);
        return ESP_ERR_TIMEOUT;
    }
    if (out) {
        out->kw1 = kw1;
        out->kw2 = kw2;
    }

    /* W4: tester replies with the bitwise inverse of KW2. */
    vTaskDelay(pdMS_TO_TICKS(KLINE_W4_MS));
    uint8_t inv_kw2 = (uint8_t)~kw2;
    if (uart_write_bytes(KL_PORT, &inv_kw2, 1) != 1) {
        return ESP_FAIL;
    }

    /* Consume our own echo, then the ECU's inverted address byte. */
    uint8_t echo = 0;
    if (uart_read_bytes(KL_PORT, &echo, 1, pdMS_TO_TICKS(200)) != 1) {
        ESP_LOGW(TAG, "addr 0x%02X: no echo of inverted KW2", addr);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t addr_echo = 0;
    if (uart_read_bytes(KL_PORT, &addr_echo, 1,
                        pdMS_TO_TICKS(KLINE_W4_MS + 50)) != 1) {
        ESP_LOGW(TAG, "addr 0x%02X: no inverted address byte from ECU", addr);
        return ESP_ERR_TIMEOUT;
    }
    if (out) {
        out->addr_echo = addr_echo;
        out->valid     = (addr_echo == (uint8_t)~addr);
    }

    ESP_LOGI(TAG, "addr 0x%02X: KW1=0x%02X KW2=0x%02X addr_echo=0x%02X (%s)",
             addr, kw1, kw2, addr_echo,
             (addr_echo == (uint8_t)~addr) ? "valid" : "unexpected");
    return ESP_OK;
#endif
}

esp_err_t kline_fast_init(void)
{
#if !CONFIG_RRP38_ALLOW_TX
    ESP_LOGE(TAG, "fast init needs transmit, disabled at compile time");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!kline_tx_possible()) {
        return ESP_ERR_INVALID_STATE;
    }

    kline_flush_rx();
    vTaskDelay(pdMS_TO_TICKS(KLINE_W5_MS));

    bitbang_begin();
    int64_t t = esp_timer_get_time();
    bitbang_set(0);
    t += 25000;
    delay_until_us(t);                    /* 25ms low  */
    bitbang_set(1);
    t += 25000;
    delay_until_us(t);                    /* 25ms high */
    bitbang_end();

    kline_flush_rx();
    ESP_LOGI(TAG, "fast init wake-up done - send StartCommunication now");
    return ESP_OK;
#endif
}

/* ------------------------------------------------------------------ */
/* Hex parsing                                                         */
/* ------------------------------------------------------------------ */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int kline_parse_hex(const char *s, uint8_t *buf, size_t max)
{
    size_t n  = 0;
    int    hi = -1;

    for (; *s; s++) {
        if (isspace((unsigned char)*s) || *s == ',' || *s == ':') {
            if (hi >= 0) {
                return -1;          /* odd nibble count in a token */
            }
            continue;
        }
        int v = hexval(*s);
        if (v < 0) {
            return -1;
        }
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= max) {
                return -1;
            }
            buf[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    return (hi >= 0) ? -1 : (int)n;
}
