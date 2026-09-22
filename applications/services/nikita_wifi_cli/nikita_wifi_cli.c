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

    FuriHalSerialHandle* serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(!serial) {
        printf("UART busy -- close the on-device WIFI app first (it holds the GPIO UART).\r\n");
        return;
    }

    FuriStreamBuffer* sb = furi_stream_buffer_alloc(1024, 1);
    furi_hal_serial_init(serial, MARAUDER_BAUD);
    furi_hal_serial_async_rx_start(serial, nikita_marauder_rx, sb, false);

    const char* cmd = furi_string_get_cstr(args);
    furi_hal_serial_tx(serial, (const uint8_t*)cmd, strlen(cmd));
    furi_hal_serial_tx(serial, (const uint8_t*)"\r\n", 2);

    // Stream the board's reply back to the shell for the read window.
    uint32_t start = furi_get_tick();
    uint32_t dur_ticks = furi_ms_to_ticks(dur_ms);
    uint8_t buf[128];
    while(furi_get_tick() - start < dur_ticks) {
        size_t n = furi_stream_buffer_receive(sb, buf, sizeof(buf), furi_ms_to_ticks(100));
        if(n) {
            printf("%.*s", (int)n, (char*)buf);
            fflush(stdout);
        }
    }

    furi_hal_serial_async_rx_stop(serial);
    furi_hal_serial_deinit(serial);
    furi_hal_serial_control_release(serial);
    furi_stream_buffer_free(sb);
    printf("\r\n");
}

int32_t nikita_wifi_cli_on_system_start(void* p) {
    UNUSED(p);
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
