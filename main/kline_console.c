/*
 * kline_console.c - interactive console commands.
 *
 * The workflow this is built around:
 *   1. 'sniff'    - passive, always safe. Confirms the hardware works.
 *   2. 'arm --yes'- deliberately unlock transmission.
 *   3. 'slowinit' - handshake with one module.
 *   4. 'send'     - raw frames, once you know what you are doing.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_console.h"
#include "esp_log.h"
#include "argtable3/argtable3.h"

#include "kline.h"

/* Candidate 5-baud init addresses for the P38A.
 *
 * 0x33 is the ISO 9141-2 standard OBD-II address and is the only entry here
 * that is specified rather than observed. The rest are the addresses most
 * often cited in the P38 community for the proprietary modules; treat them
 * as a starting point for 'scan', NOT as verified fact. Confirm each one
 * against your own car before relying on it. */
typedef struct {
    uint8_t     addr;
    const char *name;
} kline_candidate_t;

static const kline_candidate_t k_candidates[] = {
    { 0x33, "ISO 9141-2 standard (try this first)" },
    { 0x11, "candidate - engine (GEMS)"            },
    { 0x12, "candidate - gearbox"                  },
    { 0x13, "candidate - ABS"                      },
    { 0x16, "candidate - BeCM"                     },
    { 0x18, "candidate - EAS (air suspension)"     },
    { 0x1A, "candidate - HEVAC (climate)"          },
};

#define N_CANDIDATES (sizeof(k_candidates) / sizeof(k_candidates[0]))

/* ------------------------------------------------------------------ */
/* status                                                              */
/* ------------------------------------------------------------------ */

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("K-line interface\n");
    printf("  UART port     : %d\n", CONFIG_RRP38_KLINE_UART_PORT);
    printf("  TX GPIO       : %d\n", CONFIG_RRP38_KLINE_TX_GPIO);
    printf("  RX GPIO       : %d\n", CONFIG_RRP38_KLINE_RX_GPIO);
#if CONFIG_RRP38_LLINE_TX_GPIO >= 0
    printf("  L-line GPIO   : %d\n", CONFIG_RRP38_LLINE_TX_GPIO);
#else
    printf("  L-line GPIO   : disabled\n");
#endif
#if CONFIG_RRP38_EN_GPIO >= 0
    printf("  Transceiver EN: GPIO%d (high)\n", CONFIG_RRP38_EN_GPIO);
#else
    printf("  Transceiver EN: not used\n");
#endif
#if CONFIG_RRP38_INIT_DRIVER_GPIO >= 0
    printf("  Init bypass   : GPIO%d (TxD timeout workaround active)\n",
           CONFIG_RRP38_INIT_DRIVER_GPIO);
#else
    printf("  Init bypass   : none (init driven through transceiver)\n");
#endif
    printf("  Baud          : %d\n", CONFIG_RRP38_KLINE_BAUD);
#if CONFIG_RRP38_ALLOW_TX
    printf("  TX compiled   : YES\n");
    printf("  TX armed      : %s\n", kline_tx_armed() ? "YES" : "no");
#else
    printf("  TX compiled   : no (passive build - cannot transmit)\n");
#endif
    return 0;
}

/* ------------------------------------------------------------------ */
/* arm / disarm                                                        */
/* ------------------------------------------------------------------ */

static struct {
    struct arg_lit *yes;
    struct arg_end *end;
} arm_args;

static int cmd_arm(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&arm_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, arm_args.end, argv[0]);
        return 1;
    }

#if !CONFIG_RRP38_ALLOW_TX
    printf("This is a passive build. Rebuild with CONFIG_RRP38_ALLOW_TX=y\n"
           "to enable transmission - but read the README first.\n");
    return 1;
#else
    if (arm_args.yes->count == 0) {
        printf("Arming enables transmission onto the car's diagnostic line.\n"
               "Malformed traffic to the BeCM is a documented cause of\n"
               "immobiliser lockout requiring the EKA code to recover.\n\n"
               "Have your EKA code to hand, then run: arm --yes\n");
        return 1;
    }
    kline_set_tx_armed(true);
    printf("TX ARMED. Run 'disarm' when finished.\n");
    return 0;
#endif
}

static int cmd_disarm(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kline_set_tx_armed(false);
    printf("TX disarmed.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* sniff                                                               */
/* ------------------------------------------------------------------ */

static struct {
    struct arg_int *secs;
    struct arg_int *gap;
    struct arg_end *end;
} sniff_args;

static int cmd_sniff(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&sniff_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, sniff_args.end, argv[0]);
        return 1;
    }

    uint32_t secs = (sniff_args.secs->count > 0)
                        ? (uint32_t)sniff_args.secs->ival[0] : 30;
    uint32_t gap  = (sniff_args.gap->count > 0)
                        ? (uint32_t)sniff_args.gap->ival[0] : 10;

    kline_sniff(secs, gap);
    return 0;
}

/* ------------------------------------------------------------------ */
/* slowinit                                                            */
/* ------------------------------------------------------------------ */

static struct {
    struct arg_str *addr;
    struct arg_end *end;
} slowinit_args;

static void print_init_result(uint8_t addr, const kline_init_result_t *r,
                              esp_err_t err)
{
    if (err == ESP_ERR_TIMEOUT && !r->responded) {
        printf("  0x%02X : silent\n", addr);
        return;
    }
    printf("  0x%02X : sync=0x%02X kw1=0x%02X kw2=0x%02X echo=0x%02X "
           "W1=%" PRIu32 "ms %s\n",
           addr, r->sync, r->kw1, r->kw2, r->addr_echo, r->w1_actual_ms,
           r->valid ? "[VALID]" : "[partial]");
}

static int cmd_slowinit(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&slowinit_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, slowinit_args.end, argv[0]);
        return 1;
    }

    uint8_t addr = 0;
    if (kline_parse_hex(slowinit_args.addr->sval[0], &addr, 1) != 1) {
        printf("address must be a single hex byte, e.g. 33\n");
        return 1;
    }

    printf("5-baud init to 0x%02X (takes ~2.3s)...\n", addr);
    kline_init_result_t r;
    esp_err_t err = kline_slow_init(addr, &r);
    print_init_result(addr, &r, err);
    return 0;
}

/* ------------------------------------------------------------------ */
/* fastinit                                                            */
/* ------------------------------------------------------------------ */

static int cmd_fastinit(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = kline_fast_init();
    printf("fast init: %s\n", esp_err_to_name(err));
    return 0;
}

/* ------------------------------------------------------------------ */
/* scan                                                                */
/* ------------------------------------------------------------------ */

static struct {
    struct arg_str *from;
    struct arg_str *to;
    struct arg_end *end;
} scan_args;

static int cmd_scan(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&scan_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, scan_args.end, argv[0]);
        return 1;
    }

    if (!kline_tx_possible()) {
        printf("TX not available - see 'status'.\n");
        return 1;
    }

    /* No range given: walk the curated candidate list. */
    if (scan_args.from->count == 0) {
        printf("Scanning %u candidate addresses (~3s each)...\n",
               (unsigned)N_CANDIDATES);
        for (size_t i = 0; i < N_CANDIDATES; i++) {
            kline_init_result_t r;
            esp_err_t err = kline_slow_init(k_candidates[i].addr, &r);
            print_init_result(k_candidates[i].addr, &r, err);
            printf("         %s\n", k_candidates[i].name);
        }
        return 0;
    }

    uint8_t lo = 0, hi = 0;
    if (kline_parse_hex(scan_args.from->sval[0], &lo, 1) != 1 ||
        scan_args.to->count == 0 ||
        kline_parse_hex(scan_args.to->sval[0], &hi, 1) != 1) {
        printf("usage: scan [-f <hex> -t <hex>]\n");
        return 1;
    }
    if (hi < lo) {
        printf("range is backwards\n");
        return 1;
    }

    printf("Scanning 0x%02X..0x%02X (~3s each, %d addresses)...\n",
           lo, hi, hi - lo + 1);
    for (unsigned a = lo; a <= hi; a++) {
        kline_init_result_t r;
        esp_err_t err = kline_slow_init((uint8_t)a, &r);
        if (r.responded) {
            print_init_result((uint8_t)a, &r, err);
        }
    }
    printf("scan complete\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* send                                                                */
/* ------------------------------------------------------------------ */

static struct {
    struct arg_lit *csum;
    struct arg_int *wait;
    struct arg_str *bytes;
    struct arg_end *end;
} send_args;

static int cmd_send(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&send_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, send_args.end, argv[0]);
        return 1;
    }

    /* Re-join the hex tokens so both "81 12 F1" and "8112F1" work. */
    char joined[3 * KLINE_MAX_FRAME + 1];
    joined[0] = '\0';
    for (int i = 0; i < send_args.bytes->count; i++) {
        if (strlen(joined) + strlen(send_args.bytes->sval[i]) + 2 >=
            sizeof(joined)) {
            printf("too many bytes\n");
            return 1;
        }
        strcat(joined, send_args.bytes->sval[i]);
        strcat(joined, " ");
    }

    uint8_t frame[KLINE_MAX_FRAME];
    int n = kline_parse_hex(joined, frame, sizeof(frame) - 1);
    if (n <= 0) {
        printf("could not parse hex: '%s'\n", joined);
        return 1;
    }

    if (send_args.csum->count > 0) {
        frame[n] = kline_checksum(frame, (size_t)n);
        printf("appended checksum 0x%02X\n", frame[n]);
        n++;
    }

    printf("TX:");
    for (int i = 0; i < n; i++) {
        printf(" %02X", frame[i]);
    }
    printf("\n");

    esp_err_t err = kline_write(frame, (size_t)n);
    if (err != ESP_OK) {
        printf("write failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    uint32_t wait_ms = (send_args.wait->count > 0)
                           ? (uint32_t)send_args.wait->ival[0] : 500;

    uint8_t resp[KLINE_MAX_FRAME];
    int got = kline_read(resp, sizeof(resp), wait_ms, KLINE_P1_MS + 20);
    if (got <= 0) {
        printf("RX: (no response within %" PRIu32 "ms)\n", wait_ms);
        return 0;
    }

    printf("RX:");
    for (int i = 0; i < got; i++) {
        printf(" %02X", resp[i]);
    }
    uint8_t want = kline_checksum(resp, (size_t)got - 1);
    printf("   [%d bytes, checksum %s]\n", got,
           (want == resp[got - 1]) ? "OK" : "MISMATCH");
    return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

void kline_console_register(void)
{
    arm_args.yes = arg_lit0(NULL, "yes", "confirm you understand the risk");
    arm_args.end = arg_end(2);
    const esp_console_cmd_t arm_cmd = {
        .command  = "arm",
        .help     = "Unlock K-line transmission (requires --yes)",
        .hint     = NULL,
        .func     = &cmd_arm,
        .argtable = &arm_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&arm_cmd));

    const esp_console_cmd_t disarm_cmd = {
        .command = "disarm",
        .help    = "Lock K-line transmission again",
        .hint    = NULL,
        .func    = &cmd_disarm,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&disarm_cmd));

    const esp_console_cmd_t status_cmd = {
        .command = "status",
        .help    = "Show pin mapping and transmit state",
        .hint    = NULL,
        .func    = &cmd_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));

    sniff_args.secs = arg_int0("t", "time", "<s>", "capture duration (default 30)");
    sniff_args.gap  = arg_int0("g", "gap", "<ms>", "frame split gap (default 10)");
    sniff_args.end  = arg_end(2);
    const esp_console_cmd_t sniff_cmd = {
        .command  = "sniff",
        .help     = "Passively log K-line traffic (never transmits)",
        .hint     = NULL,
        .func     = &cmd_sniff,
        .argtable = &sniff_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&sniff_cmd));

    slowinit_args.addr = arg_str1(NULL, NULL, "<hex>", "ECU address, e.g. 33");
    slowinit_args.end  = arg_end(2);
    const esp_console_cmd_t slowinit_cmd = {
        .command  = "slowinit",
        .help     = "ISO 9141-2 five-baud address initialisation",
        .hint     = NULL,
        .func     = &cmd_slowinit,
        .argtable = &slowinit_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&slowinit_cmd));

    const esp_console_cmd_t fastinit_cmd = {
        .command = "fastinit",
        .help    = "KWP2000 fast init wake-up pattern",
        .hint    = NULL,
        .func    = &cmd_fastinit,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&fastinit_cmd));

    scan_args.from = arg_str0("f", "from", "<hex>", "first address");
    scan_args.to   = arg_str0("t", "to", "<hex>", "last address");
    scan_args.end  = arg_end(2);
    const esp_console_cmd_t scan_cmd = {
        .command  = "scan",
        .help     = "Try slow init across addresses; no range = candidate list",
        .hint     = NULL,
        .func     = &cmd_scan,
        .argtable = &scan_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&scan_cmd));

    send_args.csum  = arg_lit0("c", "checksum", "append sum-of-bytes checksum");
    send_args.wait  = arg_int0("w", "wait", "<ms>", "response wait (default 500)");
    send_args.bytes = arg_strn(NULL, NULL, "<hex>", 1, 64, "frame bytes");
    send_args.end   = arg_end(2);
    const esp_console_cmd_t send_cmd = {
        .command  = "send",
        .help     = "Send a raw frame and print any response",
        .hint     = NULL,
        .func     = &cmd_send,
        .argtable = &send_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&send_cmd));
}
