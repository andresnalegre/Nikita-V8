/**
 * The firmware side of the Nikita ecosystem.
 *
 * The Nikita agents (nikita-IOS over BLE, nikita-qflipper over USB, and the
 * nikita-flipper-bridge in between) all end up talking to this device. Two
 * things they need are awkward to get today: a single, parseable snapshot of
 * what the device is, and a place to keep durable user facts that does not
 * belong to one client. Both live here, so a fact remembered on the phone is
 * the same fact the desktop reads back.
 */

#include "../cli_main_commands.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_info.h>
#include <furi_hal_version.h>
#include <furi_hal_power.h>
#include <furi_hal_bt.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <toolbox/args.h>
#include <toolbox/version.h>
#include <toolbox/stream/stream.h>
#include <toolbox/stream/file_stream.h>
#include <storage/storage.h>
#include "nikita_bridge_payload.h"

#define NIKITA_DIR         EXT_PATH("nikita")
#define NIKITA_MEMORY_FILE NIKITA_DIR "/memory.txt"

// The relay mailbox. A client on BLE (nikita-IOS) cannot reach the machine the
// Flipper is plugged into -- Bluetooth carries no serial line and the phone
// runs on no shell of its own. But it CAN write a file over RPC, and the
// nikita-flipper-bridge on that machine CAN read one over USB. So a request
// left here by the phone is picked up on the far side, run, and its answer left
// back in the same folder for the phone to read. The firmware only has to own
// the folder and hand out one request at a time; the two ends do the rest.
#define NIKITA_BRIDGE_DIR NIKITA_DIR "/bridge"
#define NIKITA_BRIDGE_REQ NIKITA_BRIDGE_DIR "/req"
#define NIKITA_BRIDGE_RES NIKITA_BRIDGE_DIR "/res"

// The folders the ecosystem expects to find on a Nikita device.
static const char* const nikita_dirs[] = {
    NIKITA_DIR,
    NIKITA_DIR "/artifacts",
    NIKITA_DIR "/scripts",
    NIKITA_BRIDGE_DIR,
};

// A remembered fact is one line. Keep both the line and the file bounded so a
// runaway agent can't eat the card or the command's stack.
#define NIKITA_MEMORY_MAX_LINES 64
#define NIKITA_MEMORY_MAX_LINE  256

static void nikita_print_usage(FuriString* args) {
    cli_print_usage("nikita", "<info|init|memory>", furi_string_get_cstr(args));
    printf("\r\n"
           "  nikita info                 device snapshot for the agent\r\n"
           "  nikita init                 create /ext/nikita on the SD card\r\n"
           "  nikita memory               list remembered facts\r\n"
           "  nikita memory add <text>    remember one fact\r\n"
           "  nikita memory forget <n>    drop fact number <n>\r\n"
           "  nikita memory clear         drop all of them\r\n"
           "  nikita bridge status        show the relay mailbox\r\n"
           "  nikita bridge poll          print a pending request and consume it\r\n"
           "  nikita bridge clear         empty the mailbox\r\n"
           "  nikita install flipper-bridge  type the bridge into this Mac\r\n"
           "  nikita usb <cdc|hid|composite>  switch USB mode (composite = CDC+HID)\r\n"
           "  nikita host                 guess the host OS from how it enumerated us\r\n"
           "  nikita host reset           clear the fingerprint before a re-plug\r\n");
}

static bool nikita_ensure_dirs(Storage* storage) {
    bool ok = true;
    for(size_t i = 0; i < COUNT_OF(nikita_dirs); i++) {
        FS_Error error = storage_common_mkdir(storage, nikita_dirs[i]);
        if(error != FSE_OK && error != FSE_EXIST) ok = false;
    }
    return ok;
}

/** `nikita info` -- everything the agent would otherwise need five calls for. */
static void nikita_info(Storage* storage) {
    const Version* ver = furi_hal_version_get_firmware_version();

    printf("firmware_origin  : %s\r\n", ver ? version_get_firmware_origin(ver) : "unknown");
    printf("firmware_version : %s\r\n", ver ? version_get_version(ver) : "unknown");
    printf("firmware_branch  : %s\r\n", ver ? version_get_gitbranch(ver) : "unknown");
    printf("firmware_commit  : %s\r\n", ver ? version_get_githash(ver) : "unknown");
    printf("firmware_build   : %s\r\n", ver ? version_get_builddate(ver) : "unknown");

    const char* name = furi_hal_version_get_name_ptr();
    printf("device_name      : %s\r\n", name ? name : "unknown");
    printf("hardware_target  : f%d\r\n", furi_hal_version_get_hw_target());

    uint16_t api_major, api_minor;
    furi_hal_info_get_api_version(&api_major, &api_minor);
    printf("api_version      : %d.%d\r\n", api_major, api_minor);

    printf("battery_pct      : %d\r\n", furi_hal_power_get_pct());
    printf("charging         : %s\r\n", furi_hal_power_is_charging() ? "yes" : "no");
    printf("bluetooth        : %s\r\n", furi_hal_bt_is_active() ? "on" : "off");

    uint64_t total = 0, free = 0;
    if(storage_common_fs_info(storage, STORAGE_EXT_PATH_PREFIX, &total, &free) == FSE_OK) {
        printf("sd_total_kb      : %lu\r\n", (unsigned long)(total / 1024));
        printf("sd_free_kb       : %lu\r\n", (unsigned long)(free / 1024));
    } else {
        printf("sd_total_kb      : none\r\n");
        printf("sd_free_kb       : none\r\n");
    }

    printf(
        "nikita_dir       : %s\r\n",
        storage_common_stat(storage, NIKITA_DIR, NULL) == FSE_OK ? NIKITA_DIR : "missing");
}

/** Walk memory.txt line by line. Returns the number of lines seen. */
static size_t nikita_memory_walk(
    Storage* storage,
    void (*on_line)(size_t index, FuriString* line, void* ctx),
    void* ctx) {
    Stream* stream = file_stream_alloc(storage);
    size_t index = 0;

    if(file_stream_open(stream, NIKITA_MEMORY_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FuriString* line = furi_string_alloc();
        while(stream_read_line(stream, line)) {
            furi_string_trim(line);
            if(furi_string_empty(line)) continue;
            index++;
            if(on_line) on_line(index, line, ctx);
        }
        furi_string_free(line);
    }

    file_stream_close(stream);
    stream_free(stream);
    return index;
}

static void nikita_memory_print_line(size_t index, FuriString* line, void* ctx) {
    UNUSED(ctx);
    printf("%u. %s\r\n", (unsigned)index, furi_string_get_cstr(line));
}

typedef struct {
    size_t skip;
    Stream* out;
} NikitaMemoryRewrite;

static void nikita_memory_rewrite_line(size_t index, FuriString* line, void* ctx) {
    NikitaMemoryRewrite* rewrite = ctx;
    if(index == rewrite->skip) return;
    stream_write_format(rewrite->out, "%s\n", furi_string_get_cstr(line));
}

static void nikita_memory_list(Storage* storage) {
    if(nikita_memory_walk(storage, nikita_memory_print_line, NULL) == 0) {
        printf("Nothing remembered yet.\r\n");
    }
}

static void nikita_memory_add(Storage* storage, FuriString* args) {
    furi_string_trim(args);
    if(furi_string_empty(args)) {
        cli_print_usage("nikita memory add", "<text>", "");
        return;
    }
    if(furi_string_size(args) > NIKITA_MEMORY_MAX_LINE) {
        printf("Too long: one fact is at most %d characters.\r\n", NIKITA_MEMORY_MAX_LINE);
        return;
    }
    if(nikita_memory_walk(storage, NULL, NULL) >= NIKITA_MEMORY_MAX_LINES) {
        printf("Memory is full (%d facts). Forget something first.\r\n", NIKITA_MEMORY_MAX_LINES);
        return;
    }
    // A fact is one line, so a pasted newline would silently become two.
    furi_string_replace_all(args, "\n", " ");
    furi_string_replace_all(args, "\r", " ");

    if(!nikita_ensure_dirs(storage)) {
        printf("Cannot write to %s. Is the SD card mounted?\r\n", NIKITA_DIR);
        return;
    }

    Stream* stream = file_stream_alloc(storage);
    if(file_stream_open(stream, NIKITA_MEMORY_FILE, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        stream_write_format(stream, "%s\n", furi_string_get_cstr(args));
        printf("Remembered.\r\n");
    } else {
        printf("Cannot open %s.\r\n", NIKITA_MEMORY_FILE);
    }
    file_stream_close(stream);
    stream_free(stream);
}

static void nikita_memory_forget(Storage* storage, FuriString* args) {
    int index = 0;
    if(!args_read_int_and_trim(args, &index) || index < 1) {
        cli_print_usage("nikita memory forget", "<n>", furi_string_get_cstr(args));
        return;
    }

    size_t total = nikita_memory_walk(storage, NULL, NULL);
    if((size_t)index > total) {
        printf("There is no fact %d (%u remembered).\r\n", index, (unsigned)total);
        return;
    }

    // Rewrite into a temporary file, then swap: a power loss mid-write must not
    // take the whole memory with it.
    const char* temp_path = NIKITA_MEMORY_FILE ".tmp";
    Stream* out = file_stream_alloc(storage);
    if(!file_stream_open(out, temp_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        printf("Cannot write to %s.\r\n", NIKITA_DIR);
        file_stream_close(out);
        stream_free(out);
        return;
    }

    NikitaMemoryRewrite rewrite = {.skip = (size_t)index, .out = out};
    nikita_memory_walk(storage, nikita_memory_rewrite_line, &rewrite);
    file_stream_close(out);
    stream_free(out);

    storage_common_remove(storage, NIKITA_MEMORY_FILE);
    if(storage_common_rename(storage, temp_path, NIKITA_MEMORY_FILE) == FSE_OK) {
        printf("Forgotten.\r\n");
    } else {
        printf("Could not replace %s.\r\n", NIKITA_MEMORY_FILE);
    }
}

static void nikita_memory_clear(Storage* storage) {
    FS_Error error = storage_common_remove(storage, NIKITA_MEMORY_FILE);
    if(error == FSE_OK || error == FSE_NOT_EXIST) {
        printf("Memory cleared.\r\n");
    } else {
        printf("Could not clear %s.\r\n", NIKITA_MEMORY_FILE);
    }
}

// --- install the bridge on the host, by pretending to be a keyboard --------
//
// A zeroed machine has no way to run the bridge on its own, and the Flipper --
// a USB device -- cannot reach into the host to run code. Except as a keyboard:
// HID is the one channel a device drives the host with. So on an explicit
// "nikita install flipper-bridge", the Flipper becomes a keyboard, opens a
// terminal, types the bridge (embedded in the firmware, no download, no file to
// lose) and starts it. This is the same mechanism as the Bad USB app that
// already ships; here it is scoped to typing one thing -- this ecosystem's own
// bridge -- and only when the user asks for it by name.
//
// macOS only for now: the terminal opener and `python3` are Apple-shaped.

static void nikita_hid_tap(uint16_t key) {
    // Retry until the report is actually accepted. furi_hal_hid_kb_press/
    // release_all return false when the IN-endpoint semaphore times out (the
    // host has not drained the previous report yet); a dropped press silently
    // loses a character, and one lost character corrupts a typed script -- that
    // is what turned "capture_output=True,text=True" into a syntax error. So we
    // do not fire-and-forget: press and release each get bounded retries.
    for(int i = 0; i < 50 && !furi_hal_hid_kb_press(key); i++) furi_delay_ms(4);
    furi_delay_ms(6);
    for(int i = 0; i < 50 && !furi_hal_hid_kb_release_all(); i++) furi_delay_ms(4);
    furi_delay_ms(6);
}

static void nikita_hid_type(const char* text) {
    for(const char* p = text; *p; p++) {
        if(*p == '\n') {
            nikita_hid_tap(HID_KEYBOARD_RETURN);
            furi_delay_ms(12); // a terminal needs a breath after each line
            continue;
        }
        uint16_t key = HID_ASCII_TO_KEY(*p);
        if(key != HID_KEYBOARD_NONE) nikita_hid_tap(key);
    }
}

static void nikita_install_macos(void) {
    // Spotlight -> "Terminal" -> Enter. Cmd is LEFT_GUI.
    nikita_hid_tap(KEY_MOD_LEFT_GUI | HID_KEYBOARD_SPACEBAR);
    furi_delay_ms(700);
    nikita_hid_type("Terminal");
    furi_delay_ms(600); // let Spotlight resolve the result before Return
    nikita_hid_tap(HID_KEYBOARD_RETURN);
    furi_delay_ms(4500); // a cold Terminal launch needs well over 2.5s

    // The first keystrokes after a cold launch get dropped while the window
    // takes focus -- the original 2.5s wait lost the whole "cat > ... <<EOF"
    // line and the start of the payload, so it fell into a raw shell and every
    // line errored. Send a couple of harmless Returns first: they only make
    // empty prompts, so it is those, not the heredoc opener, that get clipped.
    nikita_hid_tap(HID_KEYBOARD_RETURN);
    furi_delay_ms(250);
    nikita_hid_tap(HID_KEYBOARD_RETURN);
    furi_delay_ms(700);

    // Write the bridge to a temp file with a quoted heredoc -- 'NIKITA_EOF' so
    // the shell does not expand a thing inside it -- then start it detached.
    nikita_hid_type("cat > /tmp/nikita_bridge.py <<'NIKITA_EOF'\n");
    nikita_hid_type(NIKITA_BRIDGE_PAYLOAD);
    nikita_hid_type("NIKITA_EOF\n");
    nikita_hid_type(
        "nohup python3 /tmp/nikita_bridge.py >/tmp/nikita_bridge.log 2>&1 &\n");
}

static void nikita_install(FuriString* args) {
    FuriString* target = furi_string_alloc();
    args_read_string_and_trim(args, target);
    const bool is_bridge = furi_string_empty(target) ||
                           furi_string_cmp(target, "flipper-bridge") == 0;
    furi_string_free(target);
    if(!is_bridge) {
        printf("usage: nikita install flipper-bridge\r\n");
        return;
    }

    if(furi_hal_usb_is_locked()) {
        printf("USB is locked (a screen may be streaming). Try again.\r\n");
        return;
    }

    printf("Installing the bridge on this Mac.\r\n");
    printf("Leave the keyboard and mouse alone for ~30s -- the Flipper is about "
           "to type it in.\r\n");
    // The CLI link you typed this on drops while the Flipper is a keyboard and
    // returns after. Let the message flush first.
    furi_delay_ms(1500);

    FuriHalUsbInterface* prev = furi_hal_usb_get_config();
    if(!furi_hal_usb_set_config(&usb_hid, NULL)) {
        furi_hal_usb_set_config(prev, NULL);
        return;
    }
    furi_delay_ms(2200); // host enumerates the keyboard

    nikita_install_macos();

    furi_delay_ms(400);
    furi_hal_usb_set_config(prev, NULL); // hand the serial link back
}

// --- relay mailbox --------------------------------------------------------
//
// Deliberately dumb: the firmware never runs anything the request asks for. It
// only holds the file and hands it over once. Whatever the request means is the
// far side's problem, on the far side's machine, under whatever the user
// allowed there. A device that executed what a file told it to would be a very
// different and much worse thing.

static void nikita_bridge_status(Storage* storage) {
    FileInfo info;
    if(storage_common_stat(storage, NIKITA_BRIDGE_REQ, &info) == FSE_OK) {
        printf("request  : %lu bytes waiting\r\n", (unsigned long)info.size);
    } else {
        printf("request  : none\r\n");
    }
    if(storage_common_stat(storage, NIKITA_BRIDGE_RES, &info) == FSE_OK) {
        printf("response : %lu bytes\r\n", (unsigned long)info.size);
    } else {
        printf("response : none\r\n");
    }
}

// Print a pending request and delete it in the same breath, so a request is
// handed to exactly one reader and never run twice.
static void nikita_bridge_poll(Storage* storage) {
    Stream* stream = file_stream_alloc(storage);
    if(!file_stream_open(stream, NIKITA_BRIDGE_REQ, FSAM_READ, FSOM_OPEN_EXISTING)) {
        file_stream_close(stream);
        stream_free(stream);
        printf("(no request)\r\n");
        return;
    }
    FuriString* line = furi_string_alloc();
    while(stream_read_line(stream, line)) {
        printf("%s", furi_string_get_cstr(line));
    }
    furi_string_free(line);
    file_stream_close(stream);
    stream_free(stream);
    printf("\r\n");

    // Consumed: drop it so the next poll does not replay the same request.
    storage_common_remove(storage, NIKITA_BRIDGE_REQ);
}

static void nikita_bridge_clear(Storage* storage) {
    storage_common_remove(storage, NIKITA_BRIDGE_REQ);
    storage_common_remove(storage, NIKITA_BRIDGE_RES);
    printf("mailbox cleared.\r\n");
}

static void nikita_bridge(Storage* storage, FuriString* args) {
    FuriString* sub = furi_string_alloc();
    if(!args_read_string_and_trim(args, sub) || furi_string_cmp(sub, "status") == 0) {
        nikita_bridge_status(storage);
    } else if(furi_string_cmp(sub, "poll") == 0) {
        nikita_bridge_poll(storage);
    } else if(furi_string_cmp(sub, "clear") == 0) {
        nikita_bridge_clear(storage);
    } else {
        printf("usage: nikita bridge <status|poll|clear>\r\n");
    }
    furi_string_free(sub);
}

static void nikita_memory(Storage* storage, FuriString* args) {
    FuriString* subcommand = furi_string_alloc();

    if(!args_read_string_and_trim(args, subcommand)) {
        nikita_memory_list(storage);
    } else if(furi_string_cmp(subcommand, "add") == 0) {
        nikita_memory_add(storage, args);
    } else if(furi_string_cmp(subcommand, "forget") == 0) {
        nikita_memory_forget(storage, args);
    } else if(furi_string_cmp(subcommand, "clear") == 0) {
        nikita_memory_clear(storage);
    } else {
        cli_print_usage("nikita memory", "<add|forget|clear>", furi_string_get_cstr(subcommand));
    }

    furi_string_free(subcommand);
}

// USB mode switch, for testing the composite CDC+HID device. "composite" keeps
// the serial CLI alive AND brings up an HID keyboard on the same cable.
static void nikita_usb(FuriString* args) {
    FuriString* mode = furi_string_alloc();
    args_read_string_and_trim(args, mode);
    FuriHalUsbInterface* target = NULL;
    if(furi_string_cmp(mode, "composite") == 0) {
        target = &usb_cdc_hid;
    } else if(furi_string_cmp(mode, "hid") == 0) {
        target = &usb_hid;
    } else if(furi_string_cmp(mode, "cdc") == 0) {
        target = &usb_cdc_single;
    } else {
        printf("usage: nikita usb <cdc|hid|composite>\r\n");
        furi_string_free(mode);
        return;
    }
    printf("switching USB to %s (CDC stays up in composite)...\r\n",
           furi_string_get_cstr(mode));
    furi_string_free(mode);
    furi_delay_ms(200); // let the line flush before re-enumerating
    furi_hal_usb_set_config(target, NULL);
}

static const char* nikita_host_os_name(FuriHalUsbHostOs os) {
    switch(os) {
    case FuriHalUsbHostOsWindows:
        return "windows";
    case FuriHalUsbHostOsMacos:
        return "macos";
    case FuriHalUsbHostOsLinux:
        return "linux";
    default:
        return "unknown";
    }
}

// Passive host-OS fingerprint from how the host enumerated us. Machine-readable
// key=value lines so the app can parse it straight off BLE/serial.
// `nikita host reset` clears it before a re-plug to rescan.
static void nikita_host(FuriString* args) {
    FuriString* sub = furi_string_alloc();
    if(args_read_string_and_trim(args, sub) && furi_string_cmp(sub, "reset") == 0) {
        furi_hal_usb_reset_host_fingerprint();
        printf("reset=1 (re-plug or re-enumerate to rescan)\r\n");
        furi_string_free(sub);
        return;
    }
    furi_string_free(sub);

    FuriHalUsbHostFingerprint fp = furi_hal_usb_get_host_fingerprint();
    printf("os=%s\r\n", nikita_host_os_name(fp.os));
    printf("ms_os_string=%d\r\n", fp.ms_os_string_requested ? 1 : 0);
    printf("serial_req=%d\r\n", fp.serial_requested ? 1 : 0);
    printf("product_req=%d\r\n", fp.product_requested ? 1 : 0);
    printf("manuf_req=%d\r\n", fp.manuf_requested ? 1 : 0);
    printf("device_desc_req=%u\r\n", (unsigned)fp.device_desc_requests);
    printf("config_desc_req=%u\r\n", (unsigned)fp.config_desc_requests);
    printf("string_req=%u\r\n", (unsigned)fp.string_requests);
    printf("first_dev_wlen=%u\r\n", (unsigned)fp.first_device_desc_wlength);
}

static void execute(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(pipe);
    UNUSED(context);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* command = furi_string_alloc();

    if(!args_read_string_and_trim(args, command) || furi_string_cmp(command, "info") == 0) {
        nikita_info(storage);
    } else if(furi_string_cmp(command, "init") == 0) {
        if(nikita_ensure_dirs(storage)) {
            printf("%s is ready.\r\n", NIKITA_DIR);
        } else {
            printf("Could not create %s. Is the SD card mounted?\r\n", NIKITA_DIR);
        }
    } else if(furi_string_cmp(command, "memory") == 0) {
        nikita_memory(storage, args);
    } else if(furi_string_cmp(command, "bridge") == 0) {
        nikita_bridge(storage, args);
    } else if(furi_string_cmp(command, "install") == 0) {
        nikita_install(args);
    } else if(furi_string_cmp(command, "usb") == 0) {
        nikita_usb(args);
    } else if(furi_string_cmp(command, "host") == 0) {
        nikita_host(args);
    } else {
        nikita_print_usage(command);
    }

    furi_string_free(command);
    furi_record_close(RECORD_STORAGE);
}

CLI_COMMAND_INTERFACE(nikita, execute, CliCommandFlagParallelSafe, 4096, CLI_APPID);
