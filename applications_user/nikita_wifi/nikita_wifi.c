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
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/text_input.h>
#include <storage/storage.h>
#include <expansion/expansion.h>
#include <string.h>

// Tee everything the board prints to the Flipper SD so Nikita (over BLE/USB)
// can read_file it, analyse it and format results -- not just watch the screen.
#define WIFI_LOG_DIR "/ext/apps_data/nikita_wifi"
#define WIFI_LOG_PATH WIFI_LOG_DIR "/last.log"
// Command mailbox: any client that can write the Flipper SD (Nikita over BLE
// with no bridge, or qFlipper over USB) drops a Marauder command here, one per
// line. While the WIFI app is open it runs them on the board and the output
// lands in last.log -- full end-to-end WiFi with nothing but a file write.
#define WIFI_CMD_PATH WIFI_LOG_DIR "/cmd"

#define WIFI_BAUD 115200
#define WIFI_OUT_MAX 4096
#define WIFI_RX_STREAM 4096
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
    {"Scan",
     {"APs", "stations", "all", "ping", "arp"},
     5,
     {"scanap", "scansta", "scanall", "pingscan", "arpscan"},
     ArgNone},
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
    WifiViewCats, // top: the categories
    WifiViewOpts, // the full list of options inside a category (every attack, etc.)
    WifiViewInput, // keyboard for commands that take an argument
    WifiViewOutput, // the board's console output
} WifiView;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* cats; // category list
    Submenu* opts; // option list for the current category
    TextInput* text_input;
    TextBox* text_box;
    FuriString* out;
    FuriStreamBuffer* rx_stream;
    FuriTimer* pump;
    FuriHalSerialHandle* serial;
    Expansion* expansion;
    Storage* storage;
    WifiView current;
    bool dirty; // out changed since last snapshot
    uint8_t snap_tick; // throttles the SD snapshot
    uint8_t cmd_tick; // throttles the command-mailbox poll
    uint32_t cat_index; // category currently open
    uint32_t pending_cat; // category+option awaiting keyboard input
    uint32_t pending_opt;
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

// Append text to the rolling in-RAM window (capped). No file I/O here -- the
// snapshot writes it out on a slow timer.
static void wifi_out_append(NikitaWifi* app, const char* data, size_t len) {
    UNUSED(len);
    furi_string_cat_str(app->out, data);
    size_t n = furi_string_size(app->out);
    if(n > WIFI_OUT_MAX) {
        furi_string_right(app->out, n - WIFI_OUT_MAX);
    }
}

// Write the current window to last.log with a quick open/write/close. We never
// hold the file open, so a reader (Nikita over BLE/USB) can always open it --
// keeping it open for write is what made her read_file hang.
static void wifi_snapshot(NikitaWifi* app) {
    if(!app->storage) return;
    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, WIFI_LOG_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, furi_string_get_cstr(app->out), furi_string_size(app->out));
        storage_file_close(f);
    }
    storage_file_free(f);
}

static void wifi_pump_timer(void* context) {
    NikitaWifi* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WifiEventPump);
}

// Command mailbox: if a client dropped commands in WIFI_CMD_PATH, run each line
// on the board (output flows to last.log via the RX pump) then consume the file.
static void wifi_check_cmd(NikitaWifi* app) {
    if(!app->storage || !app->serial) return;
    File* f = storage_file_alloc(app->storage);
    char data[512];
    size_t n = 0;
    if(storage_file_open(f, WIFI_CMD_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        n = storage_file_read(f, data, sizeof(data) - 1);
        storage_file_close(f);
    }
    storage_file_free(f);
    if(n == 0) return;
    data[n] = '\0';
    storage_common_remove(app->storage, WIFI_CMD_PATH); // consume it

    // Send each non-empty line to the board.
    char* line = data;
    while(line && *line) {
        char* nl = strchr(line, '\n');
        if(nl) *nl = '\0';
        // trim trailing CR
        size_t l = strlen(line);
        if(l && line[l - 1] == '\r') line[l - 1] = '\0';
        if(line[0] != '\0') {
            furi_string_cat_printf(app->out, "\n=== mailbox: %s ===\n", line);
            size_t sz = furi_string_size(app->out);
            if(sz > WIFI_OUT_MAX) furi_string_right(app->out, sz - WIFI_OUT_MAX);
            wifi_send(app, line);
        }
        line = nl ? nl + 1 : NULL;
    }
    wifi_snapshot(app); // record the command(s) immediately
}

static bool wifi_custom_event(void* context, uint32_t event) {
    NikitaWifi* app = context;
    if(event != WifiEventPump) return false;

    // Drain the WHOLE stream buffer this tick (a scan floods far faster than a
    // single 128-byte read could keep up with). Accumulate to out+log here, then
    // sync the file and redraw the text box ONCE below -- doing either per chunk
    // hangs the device under load.
    char buf[257];
    size_t total = 0;
    for(;;) {
        size_t got = furi_stream_buffer_receive(app->rx_stream, buf, sizeof(buf) - 1, 0);
        if(!got) break;
        buf[got] = '\0';
        wifi_out_append(app, buf, got);
        total += got;
        if(total >= 8192) break; // bound work per tick; the rest comes next tick
    }
    if(total) {
        text_box_set_text(app->text_box, furi_string_get_cstr(app->out));
        text_box_set_focus(app->text_box, TextBoxFocusEnd);
        app->dirty = true;
    }
    // Snapshot to SD ~every 1s when there's fresh output, so a reader always
    // finds a recent, CLOSED last.log.
    if(app->dirty && ++app->snap_tick >= 10) {
        app->snap_tick = 0;
        app->dirty = false;
        wifi_snapshot(app);
    }

    // Poll the command mailbox ~every 500ms (pump fires at 100ms).
    if(++app->cmd_tick >= 5) {
        app->cmd_tick = 0;
        wifi_check_cmd(app);
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
    wifi_snapshot(app); // fresh log for this command
    wifi_switch(app, WifiViewOutput);
}

static void wifi_input_done(void* context); // keyboard result -> run the command

// Run category c option o, opening the keyboard first if it takes an argument.
static void wifi_execute(NikitaWifi* app, uint32_t c, uint32_t o) {
    const WifiItem* item = &k_items[c];
    char label[56];
    snprintf(label, sizeof(label), "%s: %s", item->name, item->options[o]);

    if(item->args == ArgInput) {
        app->pending_cat = c;
        app->pending_opt = o;
        app->input_buf[0] = '\0';
        char hdr[96];
        snprintf(hdr, sizeof(hdr), "%s  (args, optional)", label);
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, hdr);
        text_input_set_result_callback(
            app->text_input, wifi_input_done, app, app->input_buf, sizeof(app->input_buf), true);
        wifi_switch(app, WifiViewInput);
        return;
    }
    wifi_run(app, label, item->commands[o]);
}

// Keyboard finished: append the typed argument to the pending command.
static void wifi_input_done(void* context) {
    NikitaWifi* app = context;
    const WifiItem* item = &k_items[app->pending_cat];
    uint32_t o = app->pending_opt;
    char cmd[160];
    char label[56];
    snprintf(label, sizeof(label), "%s: %s", item->name, item->options[o]);
    if(app->input_buf[0] != '\0') {
        snprintf(cmd, sizeof(cmd), "%s %s", item->commands[o], app->input_buf);
    } else {
        snprintf(cmd, sizeof(cmd), "%s", item->commands[o]);
    }
    wifi_run(app, label, cmd);
}

// An option was chosen inside a category.
static void wifi_opt_cb(void* context, uint32_t index) {
    NikitaWifi* app = context;
    wifi_execute(app, app->cat_index, index);
}

// Build the option list for a category -- shows EVERY option (all attacks, etc).
static void wifi_build_opts(NikitaWifi* app, uint32_t c) {
    submenu_reset(app->opts);
    submenu_set_header(app->opts, k_items[c].name);
    for(uint32_t o = 0; o < k_items[c].num_options; o++) {
        submenu_add_item(app->opts, k_items[c].options[o], o, wifi_opt_cb, app);
    }
}

// A category was chosen: single-option categories run straight away, the rest
// open a list of every option so nothing is hidden behind a scroll.
static void wifi_cat_cb(void* context, uint32_t index) {
    NikitaWifi* app = context;
    if(index >= WIFI_ITEM_COUNT) return;
    if(k_items[index].num_options <= 1) {
        wifi_execute(app, index, 0);
        return;
    }
    app->cat_index = index;
    wifi_build_opts(app, index);
    wifi_switch(app, WifiViewOpts);
}

static bool wifi_back_cb(void* context) {
    NikitaWifi* app = context;
    if(app->current == WifiViewOutput) {
        wifi_send(app, "stopscan"); // leaving a running action stops it
        // back to wherever we launched from: the option list if it had one
        wifi_switch(app, k_items[app->cat_index].num_options > 1 ? WifiViewOpts : WifiViewCats);
        return true;
    }
    if(app->current == WifiViewInput) {
        wifi_switch(app, k_items[app->pending_cat].num_options > 1 ? WifiViewOpts : WifiViewCats);
        return true;
    }
    if(app->current == WifiViewOpts) {
        wifi_switch(app, WifiViewCats);
        return true;
    }
    return false; // from the category list -> exit
}

static void wifi_build_cats(NikitaWifi* app) {
    submenu_reset(app->cats);
    submenu_set_header(app->cats, "WIFI");
    for(uint32_t c = 0; c < WIFI_ITEM_COUNT; c++) {
        submenu_add_item(app->cats, k_items[c].name, c, wifi_cat_cb, app);
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

    app->cats = submenu_alloc();
    wifi_build_cats(app);
    view_dispatcher_add_view(
        app->view_dispatcher, WifiViewCats, submenu_get_view(app->cats));

    app->opts = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, WifiViewOpts, submenu_get_view(app->opts));

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, WifiViewInput, text_input_get_view(app->text_input));

    app->text_box = text_box_alloc();
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_add_view(
        app->view_dispatcher, WifiViewOutput, text_box_get_view(app->text_box));

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // The expansion service also listens on this USART to auto-detect modules;
    // it MUST be paused while we own the line, or it furi_check-faults the
    // firmware the moment the board floods data during a scan.
    app->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(app->expansion);

    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(app->serial) {
        furi_hal_serial_init(app->serial, WIFI_BAUD);
        furi_hal_serial_async_rx_start(app->serial, wifi_rx_cb, app, false);
    }

    // Storage for the snapshot log + command mailbox (never held open).
    app->storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(app->storage, "/ext/apps_data");
    storage_common_mkdir(app->storage, WIFI_LOG_DIR);
    storage_common_remove(app->storage, WIFI_CMD_PATH); // clear any stale command
    wifi_snapshot(app); // start with an empty last.log

    app->pump = furi_timer_alloc(wifi_pump_timer, FuriTimerTypePeriodic, app);
    furi_timer_start(app->pump, furi_ms_to_ticks(100));
    return app;
}

static void nikita_wifi_free(NikitaWifi* app) {
    furi_timer_stop(app->pump);
    furi_timer_free(app->pump);

    if(app->serial) {
        // Quiet the board and let it fall silent BEFORE we release the line and
        // re-enable the expansion service -- otherwise expansion probes a still-
        // streaming USART and furi_check-faults the firmware.
        wifi_send(app, "stopscan");
        furi_delay_ms(500);
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
    }

    // Hand the USART back to the expansion service.
    if(app->expansion) {
        expansion_enable(app->expansion);
        furi_record_close(RECORD_EXPANSION);
    }

    furi_record_close(RECORD_STORAGE);

    view_dispatcher_remove_view(app->view_dispatcher, WifiViewCats);
    view_dispatcher_remove_view(app->view_dispatcher, WifiViewOpts);
    view_dispatcher_remove_view(app->view_dispatcher, WifiViewInput);
    view_dispatcher_remove_view(app->view_dispatcher, WifiViewOutput);
    submenu_free(app->cats);
    submenu_free(app->opts);
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
    wifi_switch(app, WifiViewCats);
    view_dispatcher_run(app->view_dispatcher);
    nikita_wifi_free(app);
    return 0;
}
