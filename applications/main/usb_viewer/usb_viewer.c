// USB Host Viewer -- the Flipper-side Scan Viewer.
//
// Opened by holding LEFT on the desktop. It reads the passive host fingerprint
// the USB stack collected (furi_hal_usb_get_host_fingerprint) and shows it in a
// browser-style list: a "VIEWER" title bar, then one navigable row per signal,
// with the guessed OS on top. A device can't read its host, so this is the same
// best-effort guess the phone's Scan Viewer shows -- just rendered on the
// Flipper's own screen. UP/DOWN scroll, BACK exits.
#include <furi.h>
#include <furi_hal_usb.h>
#include <gui/gui.h>
#include <input/input.h>

#define VIEWER_ROWS 9
#define VIEWER_VISIBLE 4 // rows that fit under the title bar

typedef struct {
    FuriMutex* mutex;
    int selected;
    int top;
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

// Fill labels[] / values[] from the live fingerprint. Returns the row count.
static void viewer_rows(char labels[VIEWER_ROWS][16], char values[VIEWER_ROWS][16]) {
    FuriHalUsbHostFingerprint fp = furi_hal_usb_get_host_fingerprint();
    int i = 0;
    snprintf(labels[i], 16, "OS");
    snprintf(values[i], 16, "%s", viewer_os_name(fp.os));
    i++;
    snprintf(labels[i], 16, "MS-OS 0xEE");
    snprintf(values[i], 16, "%s", fp.ms_os_string_requested ? "yes" : "no");
    i++;
    snprintf(labels[i], 16, "serial");
    snprintf(values[i], 16, "%s", fp.serial_requested ? "yes" : "no");
    i++;
    snprintf(labels[i], 16, "product");
    snprintf(values[i], 16, "%s", fp.product_requested ? "yes" : "no");
    i++;
    snprintf(labels[i], 16, "manuf");
    snprintf(values[i], 16, "%s", fp.manuf_requested ? "yes" : "no");
    i++;
    snprintf(labels[i], 16, "dev desc");
    snprintf(values[i], 16, "%u", (unsigned)fp.device_desc_requests);
    i++;
    snprintf(labels[i], 16, "cfg desc");
    snprintf(values[i], 16, "%u", (unsigned)fp.config_desc_requests);
    i++;
    snprintf(labels[i], 16, "strings");
    snprintf(values[i], 16, "%u", (unsigned)fp.string_requests);
    i++;
    snprintf(labels[i], 16, "1st wLen");
    snprintf(values[i], 16, "%u", (unsigned)fp.first_device_desc_wlength);
    i++;
}

static void viewer_draw_callback(Canvas* canvas, void* ctx) {
    ViewerState* state = ctx;
    furi_mutex_acquire(state->mutex, FuriWaitForever);
    int selected = state->selected;
    int top = state->top;
    furi_mutex_release(state->mutex);

    char labels[VIEWER_ROWS][16];
    char values[VIEWER_ROWS][16];
    viewer_rows(labels, values);

    canvas_clear(canvas);

    // Title bar -- the "VIEWER" box, echoing the file browser's title.
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 13);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 3, 10, "VIEWER");

    // Rows.
    canvas_set_font(canvas, FontSecondary);
    for(int r = 0; r < VIEWER_VISIBLE; r++) {
        int idx = top + r;
        if(idx >= VIEWER_ROWS) break;
        int y = 14 + r * 12; // row band top
        bool sel = (idx == selected);
        if(sel) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y, 128, 12);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_set_color(canvas, ColorBlack);
        }
        canvas_draw_str(canvas, 3, y + 9, labels[idx]);
        // right-align the value
        uint16_t w = canvas_string_width(canvas, values[idx]);
        canvas_draw_str(canvas, 125 - w, y + 9, values[idx]);
    }

    // Scrollbar hint on the right edge when there is more than one screen.
    canvas_set_color(canvas, ColorBlack);
    if(VIEWER_ROWS > VIEWER_VISIBLE) {
        int track_h = 51; // 64 - 13
        int bar_h = track_h * VIEWER_VISIBLE / VIEWER_ROWS;
        int bar_y = 13 + (track_h - bar_h) * top / (VIEWER_ROWS - VIEWER_VISIBLE);
        canvas_draw_box(canvas, 126, bar_y, 2, bar_h);
    }
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
    state->selected = 0;
    state->top = 0;

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, viewer_draw_callback, state);
    view_port_input_callback_set(view_port, viewer_input_callback, queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }
        if(event.type != InputTypePress && event.type != InputTypeRepeat) {
            continue;
        }
        furi_mutex_acquire(state->mutex, FuriWaitForever);
        switch(event.key) {
        case InputKeyUp:
            if(state->selected > 0) state->selected--;
            if(state->selected < state->top) state->top = state->selected;
            break;
        case InputKeyDown:
            if(state->selected < VIEWER_ROWS - 1) state->selected++;
            if(state->selected >= state->top + VIEWER_VISIBLE)
                state->top = state->selected - VIEWER_VISIBLE + 1;
            break;
        case InputKeyBack:
            running = false;
            break;
        default:
            break;
        }
        furi_mutex_release(state->mutex);
        view_port_update(view_port);
    }

    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_mutex_free(state->mutex);
    free(state);
    furi_message_queue_free(queue);
    return 0;
}
