// WIFI -- Nikita's main-menu ESP32 Marauder controller.
//
// Command breadth mirrors the real flipperzero-wifi-marauder fap
// (0xchocolate): every row is a category you scroll options on with
// left/right, OK runs the selected command over the GPIO UART, and rows
// whose command takes an argument open a keyboard first. Output streams
// into a console view. Authorised testing only.

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/text_box.h>
#include <gui/modules/text_input.h>
#include <string.h>

#define WIFI_BAUD 115200
#define WIFI_OUT_MAX 4096
#define WIFI_RX_STREAM 1024
#define WIFI_MAX_OPT 18
#define WIFI_INPUT_MAX 80

typedef enum {
    ArgNone, // send the command as-is
    ArgInput, // open the keyboard, append what's typed
} ArgKind;

typedef struct {
    const char* name;
    const char* options[WIFI_MAX_OPT];
    uint8_t num_options;
    const char* commands[WIFI_MAX_OPT];
    ArgKind args;
} WifiItem;

// The full Marauder command surface. Flipper-fap-internal rows (log view,
// "save to sdcard", script engine) are intentionally left out -- everything
// here is a real command the ESP32 board understands.
static const WifiItem k_items[] = {
    {"Scan", {"all", "ping", "arp"}, 3, {"scanall", "pingscan", "arpscan"}, ArgNone},
    {"Recon",
     {"wifi", "ble", "status", "stop"},
     4,
     {"recon wifi", "recon ble", "recon status", "recon stop"},
     ArgNone},
    {"SSID",
     {"add rand", "add name", "remove"},
     3,
     {"ssid -a -g", "ssid -a -n", "ssid -r"},
     ArgInput},
    {"List",
     {"ap", "ssid", "station", "airtag", "IPs", "probes", "bluetooth", "flipper", "pineapple",
      "multissid"},
     10,
     {"list -a", "list -s", "list -c", "list -t", "list -i", "list -p", "list -b", "list -f",
      "list -x", "list -m"},
     ArgNone},
    {"Select", {"ap", "ssid", "station"}, 3, {"select -a", "select -s", "select -c"}, ArgInput},
    {"AP Info", {"info"}, 1, {"info -a"}, ArgInput},
    {"Set MAC",
     {"rand ap", "rand sta", "clone ap", "clone sta"},
     4,
     {"randapmac", "randstamac", "cloneapmac -a", "clonestamac -s"},
     ArgInput},
    {"Join WiFi",
     {"new", "saved"},
     2,
     {"join -a", "join -s"},
     ArgInput},
    {"Clear List",
     {"ap", "ssid", "station"},
     3,
     {"clearlist -a", "clearlist -s", "clearlist -c"},
     ArgNone},
    {"Attack",
     {"deauth", "probe", "rickroll", "funny", "badmsg", "sleep", "sae flood", "csa", "quiet",
      "sour apple", "apple juice", "swiftpair", "samsung", "google", "flipper spam", "bt spam all"},
     16,
     {"attack -t deauth", "attack -t probe", "attack -t rickroll", "attack -t funny",
      "attack -t badmsg", "attack -t sleep", "attack -t sae", "attack -t csa", "attack -t quiet",
      "blespam -t sourapple", "blespam -t applejuice", "blespam -t windows", "blespam -t samsung",
      "blespam -t google", "blespam -t flipper", "blespam -t all"},
     ArgNone},
    {"Airtag", {"spoof", "sound"}, 2, {"spoofat -t", "findmy -t"}, ArgInput},
    {"Wardrive", {"run"}, 1, {"wardrive"}, ArgNone},
    {"Upload Wardrive",
     {"wdg", "wigle", "both"},
     3,
     {"upload -d wdg", "upload -d wigle", "upload -d both"},
     ArgNone},
    {"Evil Portal",
     {"start", "set html", "set AP"},
     3,
     {"evilportal -c start", "evilportal -c sethtml", "evilportal -c setap"},
     ArgInput},
    {"Targeted Attack",
     {"deauth", "manual", "karma", "badmsg", "sleep"},
     5,
     {"attack -t deauth -c", "attack -t deauth -s", "karma -p", "attack -t badmsg -c",
      "attack -t sleep -c"},
     ArgInput},
    {"Beacon Spam",
     {"ap list", "ssid list", "random"},
     3,
     {"attack -t beacon -a", "attack -t beacon -l", "attack -t beacon -r"},
     ArgNone},
    {"Port Scan",
     {"all", "ssh", "telnet", "dns", "http", "smtp", "https", "rdp"},
     8,
     {"portscan -a -t", "portscan -s ssh", "portscan -s telnet", "portscan -s dns",
      "portscan -s http", "portscan -s smtp", "portscan -s https", "portscan -s rdp"},
     ArgInput},
    {"Sniff",
     {"beacon", "deauth", "pmkid", "probe", "pwn", "raw", "bt", "skim", "airtag", "flipper",
      "flock", "meta", "mactrack", "pktcount", "pineapple", "multissid", "sae"},
     17,
     {"sniffbeacon", "sniffdeauth", "sniffpmkid", "sniffprobe", "sniffpwn", "sniffraw", "sniffbt",
      "sniffskim", "sniffbt -t airtag", "sniffbt -t flipper", "sniffbt -t flock", "sniffbt -t meta",
      "mactrack", "packetcount", "sniffpinescan", "sniffmultissid", "sniffsae"},
     ArgNone},
    {"Fox Hunt",
     {"ap", "station", "bluetooth", "findmy", "flipper", "pineapple", "multissid"},
     7,
     {"foxhunt -w", "foxhunt -s", "foxhunt -b", "foxhunt -t", "foxhunt -f", "foxhunt -p",
      "foxhunt -m"},
     ArgInput},
    {"Channel", {"get", "set"}, 2, {"channel", "channel -s"}, ArgInput},
    {"LED", {"hex", "pattern"}, 2, {"led -s", "led -p"}, ArgInput},
    {"GPS Data",
     {"tracker", "stream", "fix", "sats", "lat", "lon", "alt", "date", "accuracy", "text", "nmea"},
     11,
     {"gps -t", "gpsdata", "gps -g fix", "gps -g sat", "gps -g lat", "gps -g lon", "gps -g alt",
      "gps -g date", "gps -g accuracy", "gps -g text", "gps -g nmea"},
     ArgNone},
    {"NMEA Stream", {"run"}, 1, {"nmea"}, ArgNone},
    {"GPS POI", {"start", "mark", "end"}, 3, {"gpspoi -s", "gpspoi -m", "gpspoi -e"}, ArgNone},
    {"Settings",
     {"display", "restore", "PMKID", "Probe", "SavePCAP", "LED", "EPDeauth", "custom"},
     8,
     {"settings", "settings -r", "settings -s ForcePMKID enable", "settings -s ForceProbe enable",
      "settings -s SavePCAP enable", "settings -s EnableLED enable", "settings -s EPDeauth enable",
      "settings -s"},
     ArgInput},
    {"Shutdown WiFi", {"stop"}, 1, {"stopscan -f"}, ArgNone},
    {"List SD", {"ls"}, 1, {"ls /"}, ArgInput},
    {"Protocol Info", {"show"}, 1, {"protocolinfo"}, ArgNone},
    {"Update", {"sd"}, 1, {"update -s"}, ArgNone},
    {"Reboot", {"board"}, 1, {"reboot"}, ArgNone},
    {"Help", {"show"}, 1, {"help"}, ArgNone},
    {"Info", {"show"}, 1, {"info"}, ArgNone},
    {"SPIFFS",
     {"backup", "status", "restore"},
     3,
     {"backupspiffs", "backupstatus", "restorespiffs"},
     ArgNone},
};

#define WIFI_ITEM_COUNT (sizeof(k_items) / sizeof(k_items[0]))

typedef enum {
    WifiViewMenu,
    WifiViewInput,
    WifiViewOutput,
} WifiView;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    VariableItemList* menu;
    TextInput* text_input;
    TextBox* text_box;
    VariableItem* vi[WIFI_ITEM_COUNT];
    uint8_t sel[WIFI_ITEM_COUNT]; // selected option per row
    FuriString* out;
    FuriStreamBuffer* rx_stream;
    FuriTimer* pump;
    FuriHalSerialHandle* serial;
    WifiView current;
    uint32_t pending_index; // row awaiting keyboard input
    char input_buf[WIFI_INPUT_MAX];
} NikitaWifi;

typedef enum {
    WifiEventPump = 1,
} WifiEvent;

// ---- serial ---------------------------------------------------------------

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

static void wifi_out_append(NikitaWifi* app, const char* data) {
    furi_string_cat_str(app->out, data);
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
        wifi_out_append(app, buf);
    }
    return true;
}

// ---- ui -------------------------------------------------------------------

static void wifi_switch(NikitaWifi* app, WifiView view) {
    app->current = view;
    view_dispatcher_switch_to_view(app->view_dispatcher, view);
}

// Send the command string and show the console.
static void wifi_run(NikitaWifi* app, const char* label, const char* cmd) {
    furi_string_reset(app->out);
    furi_string_printf(app->out, "> %s\n", label);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->out));
    text_box_set_focus(app->text_box, TextBoxFocusEnd);
    wifi_send(app, cmd);
    wifi_switch(app, WifiViewOutput);
}

// Keyboard finished: append the typed argument to the pending command.
static void wifi_input_done(void* context) {
    NikitaWifi* app = context;
    const WifiItem* item = &k_items[app->pending_index];
    uint8_t opt = app->sel[app->pending_index];
    char cmd[160];
    if(app->input_buf[0] != '\0') {
        snprintf(cmd, sizeof(cmd), "%s %s", item->commands[opt], app->input_buf);
    } else {
        snprintf(cmd, sizeof(cmd), "%s", item->commands[opt]);
    }
    char label[48];
    snprintf(label, sizeof(label), "%s %s", item->name, item->options[opt]);
    wifi_run(app, label, cmd);
}

// A row was scrolled: remember which option is selected and show its label.
static void wifi_item_change(VariableItem* vitem) {
    NikitaWifi* app = variable_item_get_context(vitem);
    for(size_t i = 0; i < WIFI_ITEM_COUNT; i++) {
        if(app->vi[i] == vitem) {
            uint8_t idx = variable_item_get_current_value_index(vitem);
            app->sel[i] = idx;
            variable_item_set_current_value_text(vitem, k_items[i].options[idx]);
            break;
        }
    }
}

// OK on a row: run it, or open the keyboard first if it takes an argument.
static void wifi_item_enter(void* context, uint32_t index) {
    NikitaWifi* app = context;
    if(index >= WIFI_ITEM_COUNT) return;
    const WifiItem* item = &k_items[index];
    uint8_t opt = app->sel[index];

    if(item->args == ArgInput) {
        app->pending_index = index;
        app->input_buf[0] = '\0';
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "%s %s  (args, optional)", item->name, item->options[opt]);
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, hdr);
        text_input_set_result_callback(
            app->text_input, wifi_input_done, app, app->input_buf, sizeof(app->input_buf), true);
        wifi_switch(app, WifiViewInput);
        return;
    }

    char label[48];
    snprintf(label, sizeof(label), "%s %s", item->name, item->options[opt]);
    wifi_run(app, label, item->commands[opt]);
}

static bool wifi_back_cb(void* context) {
    NikitaWifi* app = context;
    if(app->current == WifiViewOutput) {
        wifi_send(app, "stopscan"); // leaving a running action stops it
        wifi_switch(app, WifiViewMenu);
        return true;
    }
    if(app->current == WifiViewInput) {
        wifi_switch(app, WifiViewMenu);
        return true;
    }
    return false; // from the menu -> exit
}

static void wifi_build_menu(NikitaWifi* app) {
    variable_item_list_reset(app->menu);
    for(size_t i = 0; i < WIFI_ITEM_COUNT; i++) {
        VariableItem* vi = variable_item_list_add(
            app->menu, k_items[i].name, k_items[i].num_options, wifi_item_change, app);
        variable_item_set_current_value_index(vi, 0);
        variable_item_set_current_value_text(vi, k_items[i].options[0]);
        app->vi[i] = vi;
        app->sel[i] = 0;
    }
    variable_item_list_set_enter_callback(app->menu, wifi_item_enter, app);
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

    app->menu = variable_item_list_alloc();
    wifi_build_menu(app);
    view_dispatcher_add_view(
        app->view_dispatcher, WifiViewMenu, variable_item_list_get_view(app->menu));

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, WifiViewInput, text_input_get_view(app->text_input));

    app->text_box = text_box_alloc();
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_add_view(
        app->view_dispatcher, WifiViewOutput, text_box_get_view(app->text_box));

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

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
        wifi_send(app, "stopscan");
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
    }

    view_dispatcher_remove_view(app->view_dispatcher, WifiViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, WifiViewInput);
    view_dispatcher_remove_view(app->view_dispatcher, WifiViewOutput);
    variable_item_list_free(app->menu);
    text_input_free(app->text_input);
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
