// `marauder` CLI command -- Nikita's end-to-end WiFi power.
//
// Registers a Flipper text-shell command that proxies straight to the ESP32
// Marauder on the GPIO UART. Nikita (qFlipper over USB, or the phone through
// the bridge) can run ANY Marauder command via run_cli and get the board's
// output back programmatically -- no app to open, no screen to read, no
// buttons to press:
//
//   marauder scanall
//   marauder -t 8 sniffpmkid      (read for 8s instead of the 4s default)
//   marauder stopscan
//   marauder list -a
//
// That is capture -> read -> analyse, fully in Nikita's hands. Authorised
// testing only. The command needs the GPIO USART, so it fails cleanly if the
// on-device WIFI app is open (that app holds the same UART).

#include <furi.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <cli/cli.h>
#include <toolbox/cli/cli_registry.h>
#include <toolbox/cli/cli_command.h>
#include <toolbox/args.h>
#include <expansion/expansion.h>
#include <string.h>

#define MARAUDER_BAUD 115200
#define MARAUDER_DEFAULT_MS 4000
#define MARAUDER_MAX_MS 120000

static void nikita_marauder_rx(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* context) {
    FuriStreamBuffer* sb = context;
    if(event == FuriHalSerialRxEventData) {
        uint8_t b = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(sb, &b, 1, 0);
    }
}

static void nikita_marauder_cli(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(pipe);
    UNUSED(context);

    if(furi_string_empty(args)) {
        printf("Usage: marauder [-t seconds] <command>\r\n");
        printf("  e.g. marauder scanall | marauder -t 8 sniffpmkid | marauder stopscan\r\n");
        printf("  Full ESP32 Marauder command set. Authorised testing only.\r\n");
        return;
    }

    // Optional read window: "-t <seconds>" before the command.
    uint32_t dur_ms = MARAUDER_DEFAULT_MS;
    if(furi_string_start_with_str(args, "-t ")) {
        furi_string_right(args, 3);
        int sec = 0;
        if(args_read_int_and_trim(args, &sec) && sec > 0) {
            dur_ms = (uint32_t)sec * 1000;
            if(dur_ms > MARAUDER_MAX_MS) dur_ms = MARAUDER_MAX_MS;
        }
    }
    if(furi_string_empty(args)) {
        printf("No command after -t\r\n");
        return;
    }

    // Pause the expansion module service -- it shares this USART and will
    // furi_check-fault the firmware if it probes while the board is streaming.
    Expansion* expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(expansion);

    FuriHalSerialHandle* serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(!serial) {
        printf("UART busy -- close the on-device WIFI app first (it holds the GPIO UART).\r\n");
        expansion_enable(expansion);
        furi_record_close(RECORD_EXPANSION);
        return;
    }

    FuriStreamBuffer* sb = furi_stream_buffer_alloc(4096, 1);
    furi_hal_serial_init(serial, MARAUDER_BAUD);
    furi_hal_serial_async_rx_start(serial, nikita_marauder_rx, sb, false);

    const char* cmd = furi_string_get_cstr(args);
    furi_hal_serial_tx(serial, (const uint8_t*)cmd, strlen(cmd));
    furi_hal_serial_tx(serial, (const uint8_t*)"\r\n", 2);

    // Drain the board FAST into a bounded tail buffer -- do NOT printf inside the
    // loop. Per-chunk printf on the CLI thread is slow enough that, under a heavy
    // scan flood at 115200, the UART FIFO overruns and the HAL furi_check-faults.
    // We keep only the last WINDOW bytes (enough for an AP list / result) and
    // print them once at the end.
#define MARAUDER_TAIL 3072
    static char tail[MARAUDER_TAIL]; // static: keep it off the small CLI stack
    size_t tlen = 0;
    uint8_t buf[256];

    uint32_t start = furi_get_tick();
    uint32_t dur_ticks = furi_ms_to_ticks(dur_ms);
    while(furi_get_tick() - start < dur_ticks) {
        size_t n = furi_stream_buffer_receive(sb, buf, sizeof(buf), furi_ms_to_ticks(20));
        if(!n) continue; // n <= sizeof(buf) (256) < MARAUDER_TAIL, always
        if(tlen + n > MARAUDER_TAIL) {
            size_t drop = tlen + n - MARAUDER_TAIL;
            memmove(tail, tail + drop, tlen - drop);
            tlen -= drop;
        }
        memcpy(tail + tlen, buf, n);
        tlen += n;
    }

    // Quiet the board before handing the UART back, then let it fall silent.
    furi_hal_serial_tx(serial, (const uint8_t*)"stopscan\r\n", 10);
    uint32_t q = furi_get_tick();
    while(furi_get_tick() - q < furi_ms_to_ticks(500)) {
        furi_stream_buffer_receive(sb, buf, sizeof(buf), furi_ms_to_ticks(20));
    }

    furi_hal_serial_async_rx_stop(serial);
    furi_hal_serial_deinit(serial);
    furi_hal_serial_control_release(serial);
    furi_stream_buffer_free(sb);
    expansion_enable(expansion);
    furi_record_close(RECORD_EXPANSION);

    // Now it is safe to print (board quiet, line released).
    if(tlen) printf("%.*s\r\n", (int)tlen, tail);
    else printf("(no output)\r\n");
}

int32_t nikita_wifi_cli_on_system_start(void* p) {
    UNUSED(p);

    // GAMBIARRA DEFINITIVA: the ESP32 devboard permanently occupies the GPIO
    // USART, so the expansion-module service (which probes that same line to
    // auto-detect add-ons) can NEVER coexist with it -- its probe collides with
    // the board's traffic and furi_check-faults the firmware. We disable it once
    // at boot and never re-enable it, so nothing ever touches the line but us.
    // (Cost: no auto-detect of other expansion modules, which is impossible
    // anyway while the board is plugged in.)
    Expansion* expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(expansion);
    furi_record_close(RECORD_EXPANSION);

#ifdef SRV_CLI
    CliRegistry* registry = furi_record_open(RECORD_CLI);
    cli_registry_add_command(
        registry, "marauder", CliCommandFlagDefault, nikita_marauder_cli, NULL);
    furi_record_close(RECORD_CLI);
#else
    UNUSED(nikita_marauder_cli);
#endif
    return 0;
}
