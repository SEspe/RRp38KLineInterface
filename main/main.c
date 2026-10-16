/*
 * RRp38KLineInterface - ESP32 K-line diagnostic interface for the
 * Range Rover P38A (1995-2002).
 *
 * The P38A has no CAN bus. Its modules - GEMS/Motronic engine ECU, BeCM,
 * ABS, HEVAC, EAS, gearbox - are reached over a single-wire ISO 9141
 * K-line at 10400 baud, brought out on pin 7 of the diagnostic socket.
 *
 * This firmware is a transport plus an interactive console. It deliberately
 * does NOT ship a table of GEMS parameter IDs, because the Land Rover
 * message format is proprietary and the values circulating online are not
 * reliable enough to hard-code. Capture first, decode second.
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"

#include "nvs_flash.h"

#include "kline.h"
#include "net.h"
#include "sniff.h"
#include "version.h"
#include "web_server.h"

void kline_console_register(void);

static const char *TAG = "main";

static void print_banner(void)
{
    printf("\n");
    printf("=====================================================\n");
    printf("  RRp38KLineInterface - ESP32 K-line for RR P38A\n");
    printf("  " FIRMWARE_NAME " v" FIRMWARE_VERSION "\n");
    printf("=====================================================\n");
#if CONFIG_RRP38_ALLOW_TX
    printf("  Build   : TRANSMIT CAPABLE\n");
    printf("  Safety  : run 'arm --yes' before any init or send\n");
#else
    printf("  Build   : PASSIVE (listen only)\n");
    printf("  Safety  : transmission compiled out entirely\n");
#endif
    printf("\n");
    printf("  Start with 'sniff' - it is always safe and proves\n");
    printf("  the wiring works. Type 'help' for all commands.\n");
    printf("\n");
    printf("  Before transmitting: know your EKA code.\n");
    printf("=====================================================\n\n");
}

void app_main(void)
{
    print_banner();

    ESP_ERROR_CHECK(kline_start());
    sniff_init();

    /* Web HMI: own access point always, home WiFi when configured. A
     * failure here must not take the console down with it. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err == ESP_OK && net_start() == ESP_OK) {
        web_server_start();
    } else {
        ESP_LOGE(TAG, "WiFi not started (%s); console only", esp_err_to_name(err));
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt          = "p38>";
    repl_config.max_cmdline_length = 256;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* Chips with built-in USB (C3, C6, S3) enumerate as a COM port of
     * their own; the console lives there and UART0 stays unused. */
    esp_console_dev_usb_serial_jtag_config_t usb_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb_config, &repl_config, &repl));
#else
    esp_console_dev_uart_config_t uart_config =
        ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
#endif

    ESP_ERROR_CHECK(esp_console_register_help_command());
    kline_console_register();

    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    ESP_LOGI(TAG, "console ready");
}
