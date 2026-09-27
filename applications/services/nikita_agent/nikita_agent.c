/**
 * Nikita Agent Service -- the definitive, built-for-Nikita control plane.
 *
 * A headless firmware service that makes the whole Flipper a programmable API
 * for Nikita, over ANY transport and with NO bridge:
 *
 *   - MAILBOX (BLE / file-only, no computer): Nikita writes a request to
 *     /ext/nikita/agent/req ; a persistent thread runs it and writes the answer
 *     to /ext/nikita/agent/res . This is how nikita-iOS drives the device with
 *     nothing but Bluetooth file access.
 *   - CLI (USB / qFlipper): `nagent <op> [args...]` runs the same dispatcher.
 *
 * Request/response is a tiny line format (robust on embedded, trivial for the
 * agent to write/parse), not JSON:
 *
 *   req:  op: sys.led
 *         color: magenta
 *   res:  ok: 1
 *         data: led set magenta
 *
 * This file is the CORE: the service, both transports, the dispatcher, and the
 * sys.* handlers. Each Flipper subsystem (subghz, nfc, rfid, ibutton, ir,
 * badusb, wifi) plugs in as another handler on this same contract.
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_version.h>
#include <toolbox/version.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>
#include <cli/cli.h>
#include <toolbox/cli/cli_registry.h>
#include <toolbox/cli/cli_command.h>
#include <toolbox/args.h>
#include <string.h>
#include "nikita_agent_bridge.h"

#define AGENT_DIR "/ext/nikita/agent"
#define AGENT_REQ AGENT_DIR "/req"
#define AGENT_RES AGENT_DIR "/res"
#define AGENT_OUT AGENT_DIR "/out"
// 500ms, not 200: the poll opens/reads/removes files in the same dir a client
// writes its req into over BLE. Polling too hard contends with that BLE write on
// the storage service and can stall it. 500ms keeps response latency sub-second
// while leaving the card free for the incoming write to land cleanly.
#define AGENT_POLL_MS 500
#define AGENT_REQ_MAX 2048
#define AGENT_RES_MAX 4096

typedef struct {
    Storage* storage;
    NotificationApp* notif;
    FuriThread* thread;
    volatile bool running;
} NikitaAgent;

static NikitaAgent* g_agent = NULL;

// ---- tiny request parser --------------------------------------------------

// Copy the value of "key:" from the request text into out (trimmed). Returns
// true if found. Lines are "key: value"; unknown keys are ignored.
static bool agent_get(const char* req, const char* key, char* out, size_t outsz) {
    size_t klen = strlen(key);
    const char* p = req;
    while(p && *p) {
        const char* line = p;
        const char* nl = strchr(line, '\n');
        size_t linelen = nl ? (size_t)(nl - line) : strlen(line);
        // match "key:" at line start
        if(linelen > klen && strncmp(line, key, klen) == 0 && line[klen] == ':') {
            const char* v = line + klen + 1;
            while(*v == ' ' || *v == '\t') v++;
            size_t vlen = (line + linelen) - v;
            while(vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == ' ')) vlen--;
            if(vlen >= outsz) vlen = outsz - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return true;
        }
        p = nl ? nl + 1 : NULL;
    }
    out[0] = '\0';
    return false;
}

// ---- handlers -------------------------------------------------------------
//
// Each handler writes a response body into `res` (a FuriString) as key: value
// lines and returns true on success. `ok:` is added by the dispatcher.

static void agent_led(NikitaAgent* app, const char* color) {
    if(!app->notif) return;
    const NotificationSequence* seq = &sequence_reset_rgb;
    if(!strcmp(color, "red"))
        seq = &sequence_set_only_red_255;
    else if(!strcmp(color, "green"))
        seq = &sequence_set_only_green_255;
    else if(!strcmp(color, "blue"))
        seq = &sequence_set_only_blue_255;
    else if(!strcmp(color, "off"))
        seq = &sequence_reset_rgb;
    notification_message(app->notif, seq);
}

static bool agent_dispatch(NikitaAgent* app, const char* req, FuriString* res) {
    char op[48];
    if(!agent_get(req, "op", op, sizeof(op))) {
        furi_string_cat_str(res, "data: no op\n");
        return false;
    }

    if(!strcmp(op, "ping")) {
        furi_string_cat_str(res, "data: pong\n");
        return true;

    } else if(!strcmp(op, "sys.info")) {
        furi_string_cat_printf(
            res,
            "name: %s\nfirmware: %s\nheap_free: %zu\n",
            furi_hal_version_get_name_ptr() ? furi_hal_version_get_name_ptr() : "Nikita",
            version_get_version(version_get()),
            memmgr_get_free_heap());
        if(app->storage) {
            uint64_t total = 0, free = 0;
            storage_common_fs_info(app->storage, "/ext", &total, &free);
            furi_string_cat_printf(res, "sd_free_kb: %llu\n", (unsigned long long)(free / 1024));
        }
        return true;

    } else if(!strcmp(op, "sys.led")) {
        char color[16];
        if(!agent_get(req, "color", color, sizeof(color))) strlcpy(color, "off", sizeof(color));
        agent_led(app, color);
        furi_string_cat_printf(res, "data: led %s\n", color);
        return true;

    } else if(!strcmp(op, "sys.vibro")) {
        if(app->notif) notification_message(app->notif, &sequence_single_vibro);
        furi_string_cat_str(res, "data: vibro\n");
        return true;

    } else if(!strcmp(op, "sys.notify")) {
        // Nikita's "reach out": magenta-ish blink + buzz.
        if(app->notif) {
            notification_message(app->notif, &sequence_set_only_blue_255);
            notification_message(app->notif, &sequence_single_vibro);
            notification_message(app->notif, &sequence_blink_stop);
        }
        furi_string_cat_str(res, "data: notified\n");
        return true;

    } else if(!strcmp(op, "sys.reboot")) {
        furi_string_cat_str(res, "data: rebooting\n");
        furi_hal_power_reset();
        return true;

    } else if(!strcmp(op, "hid.type")) {
        // Type arbitrary text into the plugged-in computer as a USB keyboard,
        // through the given keyboard layout (layout: fr-CH, pt-BR, ... ; default
        // en-US). Without the right layout, punctuation comes out wrong.
        char text[512];
        if(!agent_get(req, "text", text, sizeof(text))) {
            furi_string_cat_str(res, "data: hid.type needs text:\n");
            return false;
        }
        char layout[24];
        if(!agent_get(req, "layout", layout, sizeof(layout))) layout[0] = '\0';
        nkb_load_layout(app->storage, layout);
        FuriHalUsbInterface* prev = furi_hal_usb_get_config();
        if(furi_hal_usb_is_locked() || !furi_hal_usb_set_config(&usb_hid, NULL)) {
            furi_string_cat_str(res, "data: USB busy/locked\n");
            return false;
        }
        furi_delay_ms(1500);
        nkb_type(text);
        furi_delay_ms(300);
        furi_hal_usb_set_config(prev, NULL);
        furi_string_cat_str(res, "data: typed\n");
        return true;

    } else if(!strcmp(op, "bridge.install")) {
        // Ship-in-firmware bridge bootstrap, typed into the target over HID.
        char os[12];
        if(!agent_get(req, "os", os, sizeof(os))) strlcpy(os, "mac", sizeof(os));
        char layout[24];
        if(!agent_get(req, "layout", layout, sizeof(layout))) layout[0] = '\0';
        char open[8];
        // open: no  -> type into an already-open shell (e.g. RetroPie); default yes
        bool open_term = !(agent_get(req, "open", open, sizeof(open)) &&
                           (!strcmp(open, "no") || !strcmp(open, "0")));
        int rc = nikita_agent_install_bridge(app->storage, os, layout, open_term);
        if(rc == 0) {
            furi_string_cat_printf(
                res, "data: bridge install typed for %s (layout %s)\n", os,
                layout[0] ? layout : "en-US");
            return true;
        }
        furi_string_cat_printf(
            res, "data: install failed (%s)\n", rc == 1 ? "USB locked" : "USB switch failed");
        return false;
    }

    furi_string_cat_printf(res, "data: unknown op '%s'\n", op);
    return false;
}

// ---- transports -----------------------------------------------------------

// Run one request text, return the full response text in `res`.
static void agent_run(NikitaAgent* app, const char* req, FuriString* res) {
    furi_string_reset(res);
    FuriString* body = furi_string_alloc();
    bool ok = agent_dispatch(app, req, body);
    furi_string_cat_printf(res, "ok: %d\n", ok ? 1 : 0);
    furi_string_cat(res, body);
    furi_string_free(body);
}

// Write the response, RETRYING under contention. When a client (phone/qFlipper)
// keeps a BLE/RPC session OPEN, its storage traffic competes with ours: a single
// open/write can fail because the storage is momentarily busy, and then the
// answer is lost forever (the req is already consumed). So we retry into a free
// window instead of giving up. This is THE fix for "req saved but res never
// appears while connected over BLE". Returns true if the answer landed.
// Overwrite AGENT_RES with `data`, RETRYING under contention. The res file is
// NEVER deleted -- only its CONTENT changes. That is the whole trick: an RPC
// client (phone/qFlipper) caches the directory listing when it connects, so a
// file CREATED after that point reads back as "does not exist" forever. By
// keeping res ALWAYS PRESENT (created at boot, only rewritten), the client's read
// always finds it and just sees the current content. Retry handles the moment
// the storage is busy with the active session's own traffic.
static bool agent_write_res_bytes(NikitaAgent* app, const char* data, size_t sz) {
    for(int attempt = 0; attempt < 40; attempt++) {
        File* rf = storage_file_alloc(app->storage);
        bool ok = false;
        if(storage_file_open(rf, AGENT_RES, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            ok = storage_file_write(rf, data, sz) == sz;
            storage_file_close(rf);
        }
        storage_file_free(rf);
        if(ok) return true;
        furi_thread_yield();
        furi_delay_ms(100); // storage busy (active RPC session) -- back off, retry
    }
    return false;
}

static bool agent_write_res(NikitaAgent* app, FuriString* res) {
    return agent_write_res_bytes(app, furi_string_get_cstr(res), furi_string_size(res));
}

// Reset res to the sentinel the client waits past. Because res always exists,
// the client never reads "does not exist" -- it polls the CONTENT and knows the
// answer arrived when this "pending" turns into "ok:"/"data:".
static void agent_res_pending(NikitaAgent* app) {
    static const char* PENDING = "pending\n";
    agent_write_res_bytes(app, PENDING, strlen(PENDING));
}

// Mailbox: if a request file exists, run it and leave the answer in res.
static void agent_poll_mailbox(NikitaAgent* app) {
    // Reading the request also retries: an active session can make the OPEN fail
    // even though the file is there, and skipping it would drop the request.
    char* req = NULL;
    size_t n = 0;
    for(int attempt = 0; attempt < 6 && !req; attempt++) {
        File* f = storage_file_alloc(app->storage);
        if(storage_file_open(f, AGENT_REQ, FSAM_READ, FSOM_OPEN_EXISTING)) {
            req = malloc(AGENT_REQ_MAX);
            n = storage_file_read(f, req, AGENT_REQ_MAX - 1);
            storage_file_close(f);
            storage_file_free(f);
            break;
        }
        storage_file_free(f);
        // OPEN failed: either there's genuinely no request (the common case ->
        // give up quietly) or the storage is busy. We can't tell cheaply, so try
        // a couple of quick retries; if it was just "no file", these are cheap.
        if(!storage_common_exists(app->storage, AGENT_REQ)) return; // no request
        furi_delay_ms(40); // exists but busy -- back off and retry the open
    }
    if(!req) return;
    req[n] = '\0';
    storage_common_remove(app->storage, AGENT_REQ); // consume
    // OBSERVABLE: flash BLUE the instant we take a request, so you can SEE over
    // BLE that the agent is alive and picked it up (no card read needed).
    if(app->notif) notification_message(app->notif, &sequence_blink_blue_100);
    // Reset res to "pending" (NOT delete) the instant we take a request, so a
    // client polling reads "pending" (keep waiting) instead of a stale answer --
    // and the file keeps existing so the read never fails with "does not exist".
    agent_res_pending(app);

    FuriString* res = furi_string_alloc();
    agent_run(app, req, res);
    free(req);
    bool wrote = agent_write_res(app, res); // retries into a free window
    // OBSERVABLE: GREEN if the answer landed on the card, RED if every retry
    // failed (storage never freed). This tells us, live over BLE, exactly which
    // half breaks -- no guessing, no card access.
    if(app->notif)
        notification_message(
            app->notif, wrote ? &sequence_blink_green_100 : &sequence_blink_red_100);
    furi_string_free(res);
}

static int32_t agent_thread(void* context) {
    NikitaAgent* app = context;
    while(app->running) {
        agent_poll_mailbox(app);
        furi_delay_ms(AGENT_POLL_MS);
    }
    return 0;
}

// CLI: `nagent <op> [key value ...]` -- build a request and print the response.
static void agent_cli(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(pipe);
    NikitaAgent* app = context;
    if(furi_string_empty(args)) {
        printf("Usage: nagent <op> [key: value lines via mailbox]\r\n");
        printf("  ops: ping, sys.info, sys.led (color:), sys.vibro, sys.notify, sys.reboot\r\n");
        return;
    }
    // The CLI form takes just the op (and simple inline args aren't parsed here
    // -- the mailbox is the rich path). Build a minimal request.
    FuriString* req = furi_string_alloc();
    furi_string_printf(req, "op: %s\n", furi_string_get_cstr(args));
    FuriString* res = furi_string_alloc();
    agent_run(app, furi_string_get_cstr(req), res);
    printf("%s", furi_string_get_cstr(res));
    furi_string_free(req);
    furi_string_free(res);
}

// ---- lifecycle ------------------------------------------------------------

int32_t nikita_agent_on_system_start(void* p) {
    UNUSED(p);
    NikitaAgent* app = malloc(sizeof(NikitaAgent));
    memset(app, 0, sizeof(NikitaAgent));
    g_agent = app;

    app->storage = furi_record_open(RECORD_STORAGE);
    app->notif = furi_record_open(RECORD_NOTIFICATION);
    storage_common_mkdir(app->storage, "/ext/nikita");
    storage_common_mkdir(app->storage, AGENT_DIR);
    storage_common_mkdir(app->storage, AGENT_OUT);
    storage_common_remove(app->storage, AGENT_REQ); // drop any stale request
    // Create res NOW with the "pending" sentinel and NEVER delete it again. It must
    // exist before any client connects and caches the directory, or that client
    // will read it as "does not exist" forever. From here on the agent only ever
    // OVERWRITES its content (pending <-> answer), never removes it.
    agent_res_pending(app);

    app->running = true;
    app->thread = furi_thread_alloc_ex("NikitaAgent", 4096, agent_thread, app);
    // HIGH priority: an active BLE/RPC session (qFlipper/phone streaming the
    // screen) is CPU-heavy and was starving this poll at Normal priority -- that's
    // why the mailbox worked over serial but went silent over BLE. Give the agent
    // enough priority to get scheduled and land its storage I/O even under load.
    furi_thread_set_priority(app->thread, FuriThreadPriorityHigh);
    furi_thread_start(app->thread);

#ifdef SRV_CLI
    CliRegistry* registry = furi_record_open(RECORD_CLI);
    cli_registry_add_command(registry, "nagent", CliCommandFlagDefault, agent_cli, app);
    furi_record_close(RECORD_CLI);
#endif
    return 0;
}
