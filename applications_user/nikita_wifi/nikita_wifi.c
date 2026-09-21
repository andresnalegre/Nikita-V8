/*
 * Nikita WIFI -- her hands on the airwaves.
 *
 * Drives an ESP32 Marauder board wired to the Flipper's GPIO UART (115200 8N1
 * on the default USART pins, TX=13 / RX=14). You pick an action; she sends the
 * Marauder command and streams the board's reply back onto the screen. Stop is
 * always one tap away, and leaving the app stops any running scan/attack so the
 * board is left quiet.
 *
 * Authorised testing only -- your own networks, or ones you're allowed to test.
 * Nikita finds and shows; you decide.
 */

#include <furi.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <string.h>

#define WIFI_BAUD 115200
#define WIFI_RX_STREAM 2048
#define WIFI_OUT_MAX 2048 // rolling window of board output shown on screen

typedef enum {
    WifiViewMenu,
    WifiViewChannel,
    WifiViewOutput,
} WifiView;

// Order matches wifi_build_menu(). Everything the real ESP32 Marauder exposes
// over serial is reachable here so Nikita isn't driving the board blind.
typedef enum {
    WifiCmdScanAP,
    WifiCmdScanSta,
    WifiCmdListAP,
    WifiCmdListSta,
    WifiCmdChannel, // opens the channel picker view
    WifiCmdSniffBeacon,
    WifiCmdSniffProbe,
    WifiCmdSniffDeauth,
    WifiCmdSniffPMKID,
    WifiCmdSniffPwn,
    WifiCmdSniffRaw,
    WifiCmdSniffEsp,
    WifiCmdAttackDeauth,
    WifiCmdAttackDeauthTgt,
    WifiCmdAttackBeaconList,
    WifiCmdAttackBeaconRand,
    WifiCmdAttackBeaconAp,
    WifiCmdAttackProbe,
    WifiCmdRickroll,
    WifiCmdWardrive,
    WifiCmdClearAP,
    WifiCmdClearSta,
    WifiCmdReboot,
    WifiCmdStop,
} WifiCmd;

// 2.4 GHz channels 1..14; the picker's item index + 1 == channel number.
#define WIFI_CHANNELS 14

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Submenu* chan_menu;
    TextBox* text_box;
    FuriString* out; // accumulated board output (bounded)
    FuriStreamBuffer* rx_stream; // ISR -> GUI byte pipe
    FuriTimer* pump; // drains rx_stream on the GUI thread
    FuriHalSerialHandle* serial;
    WifiView current;
} NikitaWifi;

typedef enum {
    WifiEventPump = 1,
} WifiEvent;

// ---- serial ---------------------------------------------------------------

// ISR context: keep it to shovelling the byte into the stream buffer.
static void wifi_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    NikitaWifi* app = context;
    if(event == FuriHalSerialRxEventData) {
        uint8_t b = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(app->rx_stream, &b, 1, 0);
    }
}

static void wifi_send(NikitaWifi* app, const char* cmd) {
    if(!app->serial) return;
    furi_hal_serial_tx(app->serial, (const uint8_t*)cmd, strlen(cmd));
    furi_hal_serial_tx(app->serial, (const uint8_t*)"\n", 1);
}

// Append bytes to the rolling output window, trimming from the front so we keep
// only the most recent WIFI_OUT_MAX characters (the screen shows the tail).
static void wifi_out_append(NikitaWifi* app, const char* data, size_t len) {
    furi_string_cat_str(app->out, data);
    UNUSED(len);
    size_t n = furi_string_size(app->out);
    if(n > WIFI_OUT_MAX) {
        furi_string_right(app->out, n - WIFI_OUT_MAX);
    }
    text_box_set_text(app->text_box, furi_string_get_cstr(app->out));
    text_box_set_focus(app->text_box, TextBoxFocusEnd);
}

static void wifi_pump_timer(void* context) {
    NikitaWifi* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WifiEventPump);
}

static bool wifi_custom_event(void* context, uint32_t event) {
    NikitaWifi* app = context;
    if(event != WifiEventPump) return false;
    char buf[129];
    size_t got = furi_stream_buffer_receive(app->rx_stream, buf, sizeof(buf) - 1, 0);
    if(got > 0) {
        buf[got] = '\0';
        wifi_out_append(app, buf, got);
    }
    return true;
}

// ---- ui -------------------------------------------------------------------

static void wifi_switch(NikitaWifi* app, WifiView view) {
    app->current = view;
    view_dispatcher_switch_to_view(app->view_dispatcher, view);
}

static void wifi_run_cmd(NikitaWifi* app, const char* label, const char* cmd) {
    furi_string_reset(app->out);
    furi_string_printf(app->out, "> %s\n", label);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->out));
    text_box_set_focus(app->text_box, TextBoxFocusEnd);
    wifi_send(app, cmd);
    wifi_switch(app, WifiViewOutput);
}

static void wifi_menu_cb(void* context, uint32_t index) {
    NikitaWifi* app = context;
    switch(index) {
    case WifiCmdScanAP: wifi_run_cmd(app, "Scan APs", "scanap"); break;
    case WifiCmdScanSta: wifi_run_cmd(app, "Scan stations", "scansta"); break;
    case WifiCmdListAP: wifi_run_cmd(app, "List APs", "list -a"); break;
    case WifiCmdListSta: wifi_run_cmd(app, "List stations", "list -s"); break;
    case WifiCmdChannel: wifi_switch(app, WifiViewChannel); break;
    case WifiCmdSniffBeacon: wifi_run_cmd(app, "Sniff beacon", "sniffbeacon"); break;
    case WifiCmdSniffProbe: wifi_run_cmd(app, "Sniff probe", "sniffprobe"); break;
    case WifiCmdSniffDeauth: wifi_run_cmd(app, "Sniff deauth", "sniffdeauth"); break;
    case WifiCmdSniffPMKID: wifi_run_cmd(app, "Sniff PMKID", "sniffpmkid"); break;
    case WifiCmdSniffPwn: wifi_run_cmd(app, "Sniff pwnagotchi", "sniffpwn"); break;
    case WifiCmdSniffRaw: wifi_run_cmd(app, "Sniff raw", "sniffraw"); break;
    case WifiCmdSniffEsp: wifi_run_cmd(app, "Sniff ESP", "sniffesp"); break;
    case WifiCmdAttackDeauth: wifi_run_cmd(app, "Deauth all", "attack -t deauth"); break;
    case WifiCmdAttackDeauthTgt:
        wifi_run_cmd(app, "Deauth targeted", "attack -t deauth -c");
        break;
    case WifiCmdAttackBeaconList:
        wifi_run_cmd(app, "Beacon (list)", "attack -t beacon -l");
        break;
    case WifiCmdAttackBeaconRand:
        wifi_run_cmd(app, "Beacon (random)", "attack -t beacon -r");
        break;
    case WifiCmdAttackBeaconAp:
        wifi_run_cmd(app, "Beacon (AP clone)", "attack -t beacon -a");
        break;
    case WifiCmdAttackProbe: wifi_run_cmd(app, "Probe flood", "attack -t probe"); break;
    case WifiCmdRickroll: wifi_run_cmd(app, "Rickroll", "attack -t rickroll"); break;
    case WifiCmdWardrive: wifi_run_cmd(app, "Wardrive", "wardrive"); break;
    case WifiCmdClearAP: wifi_run_cmd(app, "Clear APs", "clearlist -a"); break;
    case WifiCmdClearSta: wifi_run_cmd(app, "Clear stations", "clearlist -s"); break;
    case WifiCmdReboot: wifi_run_cmd(app, "Reboot board", "reboot"); break;
    case WifiCmdStop: wifi_run_cmd(app, "Stop", "stopscan"); break;
    default: break;
    }
}

// Channel picker: item index i -> channel (i + 1).
static void wifi_chan_cb(void* context, uint32_t index) {
    NikitaWifi* app = context;
    char label[16];
    char cmd[20];
    snprintf(label, sizeof(label), "Channel %lu", (unsigned long)(index + 1));
    snprintf(cmd, sizeof(cmd), "channel -s %lu", (unsigned long)(index + 1));
    wifi_run_cmd(app, label, cmd);
}

static bool wifi_back_cb(void* context) {
    NikitaWifi* app = context;
    if(app->current == WifiViewOutput) {
        wifi_send(app, "stopscan"); // leaving a running action stops it
        wifi_switch(app, WifiViewMenu);
        return true;
    }
    if(app->current == WifiViewChannel) {
        wifi_switch(app, WifiViewMenu);
        return true;
    }
    return false; // from the menu -> exit
}

static void wifi_build_menu(NikitaWifi* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Nikita WIFI");
    submenu_add_item(app->submenu, "Scan APs", WifiCmdScanAP, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Scan stations", WifiCmdScanSta, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "List APs", WifiCmdListAP, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "List stations", WifiCmdListSta, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Channel...", WifiCmdChannel, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Sniff beacon", WifiCmdSniffBeacon, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Sniff probe", WifiCmdSniffProbe, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Sniff deauth", WifiCmdSniffDeauth, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Sniff PMKID", WifiCmdSniffPMKID, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Sniff pwnagotchi", WifiCmdSniffPwn, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Sniff raw", WifiCmdSniffRaw, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Sniff ESP", WifiCmdSniffEsp, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Attack: deauth all", WifiCmdAttackDeauth, wifi_menu_cb, app);
    submenu_add_item(
        app->submenu, "Attack: deauth targeted", WifiCmdAttackDeauthTgt, wifi_menu_cb, app);
    submenu_add_item(
        app->submenu, "Attack: beacon list", WifiCmdAttackBeaconList, wifi_menu_cb, app);
    submenu_add_item(
        app->submenu, "Attack: beacon random", WifiCmdAttackBeaconRand, wifi_menu_cb, app);
    submenu_add_item(
        app->submenu, "Attack: beacon AP clone", WifiCmdAttackBeaconAp, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Attack: probe flood", WifiCmdAttackProbe, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Attack: rickroll", WifiCmdRickroll, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Wardrive", WifiCmdWardrive, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Clear AP list", WifiCmdClearAP, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Clear station list", WifiCmdClearSta, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Reboot board", WifiCmdReboot, wifi_menu_cb, app);
    submenu_add_item(app->submenu, "Stop", WifiCmdStop, wifi_menu_cb, app);
}

static void wifi_build_chan_menu(NikitaWifi* app) {
    submenu_reset(app->chan_menu);
    submenu_set_header(app->chan_menu, "Set channel");
    char label[16];
    for(uint32_t i = 0; i < WIFI_CHANNELS; i++) {
        snprintf(label, sizeof(label), "Channel %lu", (unsigned long)(i + 1));
        submenu_add_item(app->chan_menu, label, i, wifi_chan_cb, app);
    }
}

// ---- lifecycle ------------------------------------------------------------

static NikitaWifi* nikita_wifi_alloc(void) {
    NikitaWifi* app = malloc(sizeof(NikitaWifi));
    memset(app, 0, sizeof(NikitaWifi));

    app->gui = furi_record_open(RECORD_GUI);
    app->out = furi_string_alloc();
    app->rx_stream = furi_stream_buffer_alloc(WIFI_RX_STREAM, 1);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, wifi_custom_event);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, wifi_back_cb);

    app->submenu = submenu_alloc();
    wifi_build_menu(app);
    view_dispatcher_add_view(app->view_dispatcher, WifiViewMenu, submenu_get_view(app->submenu));

    app->chan_menu = submenu_alloc();
    wifi_build_chan_menu(app);
    view_dispatcher_add_view(
        app->view_dispatcher, WifiViewChannel, submenu_get_view(app->chan_menu));

    app->text_box = text_box_alloc();
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_add_view(app->view_dispatcher, WifiViewOutput, text_box_get_view(app->text_box));

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Open the GPIO UART to the ESP32 board and start listening.
    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(app->serial) {
        furi_hal_serial_init(app->serial, WIFI_BAUD);
        furi_hal_serial_async_rx_start(app->serial, wifi_rx_cb, app, false);
    }

    app->pump = furi_timer_alloc(wifi_pump_timer, FuriTimerTypePeriodic, app);
    furi_timer_start(app->pump, furi_ms_to_ticks(100));
    return app;
}

static void nikita_wifi_free(NikitaWifi* app) {
    furi_timer_stop(app->pump);
    furi_timer_free(app->pump);

    if(app->serial) {
        wifi_send(app, "stopscan"); // leave the board quiet
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
    }

    view_dispatcher_remove_view(app->view_dispatcher, WifiViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, WifiViewChannel);
    view_dispatcher_remove_view(app->view_dispatcher, WifiViewOutput);
    submenu_free(app->submenu);
    submenu_free(app->chan_menu);
    text_box_free(app->text_box);
    view_dispatcher_free(app->view_dispatcher);

    furi_stream_buffer_free(app->rx_stream);
    furi_string_free(app->out);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t nikita_wifi_app(void* p) {
    UNUSED(p);
    NikitaWifi* app = nikita_wifi_alloc();
    wifi_switch(app, WifiViewMenu);
    view_dispatcher_run(app->view_dispatcher);
    nikita_wifi_free(app);
    return 0;
}
