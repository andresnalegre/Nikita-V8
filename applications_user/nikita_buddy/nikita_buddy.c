/*
 * Nikita Buddy -- talk to Nikita from the Flipper itself.
 *
 * The Flipper has no internet, so it cannot reach the Kimi API on its own. What
 * it can do is leave a question on its own SD card and let whichever companion
 * is connected -- the iPhone app or qFlipper, both of which hold the API key and
 * relay over their own link -- pick it up, run the turn, and write the answer
 * back. This app is that surface: compose a prompt (on-screen keyboard or a
 * quick command), drop it in the mailbox, and show the reply when it lands.
 *
 * Mailbox, under /ext/nikita/buddy/ (the same card Nikita's memory and plan
 * already live on):
 *   req.json  {"id":<n>,"text":"<prompt>","ts":<secs>}   -- written here
 *   res.json  {"id":<n>,"text":"<reply>","done":true}    -- written by a client
 * A reply belongs to our request when res.id == req.id. Until then we wait, and
 * after a few seconds with no answer we say plainly what is missing: a connected
 * companion with Nikita switched on.
 *
 * Thin client on purpose. No model, no network, no BLE stack here -- just the
 * card. The intelligence is in the companion; this is the Flipper's voice into
 * the same Nikita the phone and desktop talk to.
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/widget.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <string.h>
#include <stdlib.h>

#define NB_DIR "/ext/nikita"
#define NB_BUDDY_DIR "/ext/nikita/buddy"
#define NB_REQ_PATH "/ext/nikita/buddy/req.json"
#define NB_RES_PATH "/ext/nikita/buddy/res.json"
// The shared "+" store, written by the phone and qFlipper.
#define NB_EXTRAS_PATH "/ext/nikita/extras.json"

#define NB_PROMPT_MAX 512
#define NB_REPLY_MAX 2048
#define NB_POLL_MS 1000
// After this many seconds with no reply, tell the user what is missing.
#define NB_HINT_AFTER_S 25

typedef enum {
    NbViewMenu,
    NbViewText,
    NbViewResult,
} NbView;

// Quick commands are loaded from the synced /ext/nikita/extras.json (shared by
// the phone and qFlipper), so a Quick command added on either shows up here.
// Bounded for a small device; built-ins are used when the file has none.
#define NB_MAX_QUICK 8
#define NB_QLABEL_CAP 48
#define NB_QPROMPT_CAP 256

typedef enum {
    NbMenuType = 0, // free text
    NbMenuQuick1 = 1, // quick command i uses NbMenuQuick1 + i
    NbMenuAbout = 100,
} NbMenuItem;

typedef enum {
    NbCustomPoll = 1,
} NbCustomEvent;

typedef struct {
    Gui* gui;
    Storage* storage;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextInput* text_input;
    Widget* widget;
    FuriTimer* poll_timer;

    char prompt[NB_PROMPT_MAX];
    char reply[NB_REPLY_MAX];
    uint32_t req_id; // id of the request we are waiting on
    uint32_t next_id; // monotonic id source
    uint32_t waited_s; // seconds spent waiting for the current reply
    bool answered;
    bool hint_shown;   // the "no answer yet" hint has been drawn once
    NbView current_view; // tracked ourselves; the API has no getter

    // Quick commands loaded from the synced extras.json (or the built-ins).
    char quick_labels[NB_MAX_QUICK][NB_QLABEL_CAP];
    char quick_prompts[NB_MAX_QUICK][NB_QPROMPT_CAP];
    size_t quick_count;
} NikitaBuddy;

// Switch view and remember where we are, so Back knows menu-vs-subview.
static void nb_switch(NikitaBuddy* app, NbView view);
static void nb_text_done_cb(void* context);
static void nb_open_text_input(NikitaBuddy* app);
static void nb_ask_again_cb(GuiButtonType result, InputType type, void* context);

// ---- Quick commands ------------------------------------------------------
// Prompts you can send without typing. Loaded from the synced extras.json (see
// nb_load_quick), with built-in fallbacks. Kept short and genuinely useful on a
// device with a D-pad for a keyboard.

// ---- JSON helpers (tiny, purpose-built) ----------------------------------

// Append `in` to `out` (size cap `cap`) as a JSON string body, escaping the few
// characters that would otherwise break the file. Not a general encoder -- just
// enough for a prompt typed on a Flipper.
static void nb_json_escape_into(FuriString* out, const char* in) {
    for(const char* p = in; *p; ++p) {
        char c = *p;
        switch(c) {
        case '"':
            furi_string_cat_str(out, "\\\"");
            break;
        case '\\':
            furi_string_cat_str(out, "\\\\");
            break;
        case '\n':
            furi_string_cat_str(out, "\\n");
            break;
        case '\r':
            break;
        case '\t':
            furi_string_cat_str(out, "\\t");
            break;
        default:
            furi_string_push_back(out, c);
            break;
        }
    }
}

// Pull an unsigned integer that follows a "key" in `json`. Returns false if the
// key is absent.
static bool nb_json_uint(const char* json, const char* key, uint32_t* out) {
    const char* at = strstr(json, key);
    if(!at) return false;
    at += strlen(key);
    // skip to the first digit (past ": " etc.)
    while(*at && (*at < '0' || *at > '9')) {
        if(*at == '}' || *at == '"') break;
        ++at;
    }
    if(*at < '0' || *at > '9') return false;
    uint32_t v = 0;
    while(*at >= '0' && *at <= '9') {
        v = v * 10 + (uint32_t)(*at - '0');
        ++at;
    }
    *out = v;
    return true;
}

// Copy the string value of "text":"..." out of `json` into `out`, unescaping
// the same handful of sequences we produce. Returns false if absent.
static bool nb_json_text(const char* json, char* out, size_t cap) {
    const char* at = strstr(json, "\"text\"");
    if(!at) return false;
    at = strchr(at, ':');
    if(!at) return false;
    ++at;
    while(*at == ' ') ++at;
    if(*at != '"') return false;
    ++at;
    size_t n = 0;
    while(*at && *at != '"' && n + 1 < cap) {
        if(*at == '\\') {
            ++at;
            char c = *at;
            if(c == 'n')
                out[n++] = '\n';
            else if(c == 't')
                out[n++] = '\t';
            else if(c == '"')
                out[n++] = '"';
            else if(c == '\\')
                out[n++] = '\\';
            else if(c == 0)
                break;
            else
                out[n++] = c;
            if(*at) ++at;
        } else {
            out[n++] = *at++;
        }
    }
    out[n] = '\0';
    return true;
}

// ---- Storage -------------------------------------------------------------

static bool nb_write_file(NikitaBuddy* app, const char* path, const char* data, size_t len) {
    storage_common_mkdir(app->storage, NB_DIR);
    storage_common_mkdir(app->storage, NB_BUDDY_DIR);
    File* f = storage_file_alloc(app->storage);
    bool ok = false;
    if(storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ok = storage_file_write(f, data, len) == len;
    }
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}

// Reads a file into `buf` (NUL-terminated, capped). Returns bytes read, or -1.
static int nb_read_file(NikitaBuddy* app, const char* path, char* buf, size_t cap) {
    File* f = storage_file_alloc(app->storage);
    int n = -1;
    if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        size_t r = storage_file_read(f, buf, cap - 1);
        buf[r] = '\0';
        n = (int)r;
    }
    storage_file_close(f);
    storage_file_free(f);
    return n;
}

// Read the string value that follows the Nth occurrence of "key" (i.e.
// "key"...:"value") starting at *cursor; copies the unescaped value into out and
// advances *cursor past it. Returns false when there is no further occurrence.
// A tiny, forgiving scanner -- enough for the flat quickCommands objects, not a
// general JSON parser.
static bool nb_json_next_string(const char** cursor, const char* key, char* out, size_t cap) {
    const char* p = *cursor;
    size_t klen = strlen(key);
    // Find `"key"`.
    while((p = strchr(p, '"')) != NULL) {
        if(strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            p += 1 + klen + 1;
            break;
        }
        p++;
    }
    if(!p) return false;
    // Skip to the colon and the opening quote of the value.
    p = strchr(p, ':');
    if(!p) return false;
    p++;
    while(*p == ' ' || *p == '\t') p++;
    if(*p != '"') return false;
    p++;
    // Copy the value, unescaping the handful of sequences we emit.
    size_t o = 0;
    while(*p && *p != '"') {
        char c = *p++;
        if(c == '\\' && *p) {
            char e = *p++;
            c = (e == 'n') ? '\n' : (e == 't' ? '\t' : e);
        }
        if(o + 1 < cap) out[o++] = c;
    }
    out[o] = '\0';
    if(*p == '"') p++;
    *cursor = p;
    return true;
}

// Fill the built-in quick commands (used when extras.json has none).
static void nb_load_quick_builtins(NikitaBuddy* app) {
    static const char* const labels[] = {
        "Summarize our chat",
        "What do you know about me?",
        "My Flipper status",
    };
    static const char* const prompts[] = {
        "Summarize in a few lines what we talked about recently.",
        "What do you remember about me? List it in short bullet points.",
        "Give me a short status of my Flipper: firmware, SD space, and installed apps.",
    };
    app->quick_count = 3;
    for(size_t i = 0; i < 3; i++) {
        strlcpy(app->quick_labels[i], labels[i], NB_QLABEL_CAP);
        strlcpy(app->quick_prompts[i], prompts[i], NB_QPROMPT_CAP);
    }
}

// Load quick commands from the shared extras.json so the firmware reflects what
// was added on the phone or qFlipper. Pairs each "label" with the following
// "prompt" (both clients emit label before prompt). Falls back to the built-ins.
static void nb_load_quick(NikitaBuddy* app) {
    app->quick_count = 0;
    char* buf = malloc(8192);
    if(buf) {
        int n = nb_read_file(app, NB_EXTRAS_PATH, buf, 8192);
        if(n > 0) {
            const char* cur = buf;
            char label[NB_QLABEL_CAP];
            char prompt[NB_QPROMPT_CAP];
            while(app->quick_count < NB_MAX_QUICK &&
                  nb_json_next_string(&cur, "label", label, sizeof(label)) &&
                  nb_json_next_string(&cur, "prompt", prompt, sizeof(prompt))) {
                if(label[0] == '\0' || prompt[0] == '\0') continue;
                strlcpy(app->quick_labels[app->quick_count], label, NB_QLABEL_CAP);
                strlcpy(app->quick_prompts[app->quick_count], prompt, NB_QPROMPT_CAP);
                app->quick_count++;
            }
        }
        free(buf);
    }
    if(app->quick_count == 0) nb_load_quick_builtins(app);
}

// Drop the request in the mailbox and clear any stale reply so a leftover
// res.json from a previous question cannot be mistaken for this one's answer.
static void nb_send_prompt(NikitaBuddy* app, const char* text) {
    strlcpy(app->prompt, text, sizeof(app->prompt));
    app->req_id = ++app->next_id;
    app->waited_s = 0;
    app->answered = false;
    app->hint_shown = false;
    app->reply[0] = '\0';

    FuriString* json = furi_string_alloc();
    furi_string_printf(json, "{\"id\":%lu,\"text\":\"", (unsigned long)app->req_id);
    nb_json_escape_into(json, text);
    furi_string_cat_printf(
        json, "\",\"ts\":%lu}", (unsigned long)furi_hal_rtc_get_timestamp());
    nb_write_file(app, NB_REQ_PATH, furi_string_get_cstr(json), furi_string_size(json));
    furi_string_free(json);

    // Wipe the old reply (best-effort: a plain overwrite with a non-matching id).
    nb_write_file(app, NB_RES_PATH, "{\"id\":0}", 8);
}

// ---- Result view rendering ----------------------------------------------

static void nb_show_result(NikitaBuddy* app) {
    widget_reset(app->widget);
    FuriString* body = furi_string_alloc();

    if(app->answered) {
        // A little chat context: what you asked, then Nikita's reply.
        furi_string_cat_str(body, "> ");
        furi_string_cat_str(body, app->prompt);
        furi_string_cat_str(body, "\n\n");
        furi_string_cat_str(body, app->reply);
    } else {
        furi_string_cat_str(body, "Asked Nikita...\n\n\"");
        // A short echo of what was asked, so the wait has context.
        char echo[80];
        strlcpy(echo, app->prompt, sizeof(echo));
        furi_string_cat_str(body, echo);
        if(strlen(app->prompt) >= sizeof(echo) - 1) furi_string_cat_str(body, "...");
        furi_string_cat_str(body, "\"\n\n");
        if(app->waited_s >= NB_HINT_AFTER_S) {
            // The UX the whole design turns on: say what is missing.
            furi_string_cat_str(
                body,
                "Still no answer.\nThis needs the iPhone app or\nqFlipper connected with Nikita on.\n"
                "Your question is saved; it will be\nanswered as soon as one connects.");
        } else {
            furi_string_cat_str(body, "Waiting for a reply...");
        }
    }

    // Leave room at the bottom for the Ask button once there is a reply.
    widget_add_text_scroll_element(
        app->widget, 0, 0, 128, app->answered ? 52 : 64, furi_string_get_cstr(body));
    if(app->answered) {
        widget_add_button_element(
            app->widget, GuiButtonTypeCenter, "Ask", nb_ask_again_cb, app);
    }
    furi_string_free(body);
}

static void nb_switch(NikitaBuddy* app, NbView view) {
    app->current_view = view;
    view_dispatcher_switch_to_view(app->view_dispatcher, view);
}

// Open the on-screen keyboard for a (follow-up) message. The relay keeps the
// conversation context on its side, so this just needs to send the next line.
static void nb_open_text_input(NikitaBuddy* app) {
    app->prompt[0] = '\0';
    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, "Ask Nikita");
    text_input_set_result_callback(
        app->text_input, nb_text_done_cb, app, app->prompt, sizeof(app->prompt), true);
    nb_switch(app, NbViewText);
}

// The "Ask" button on the reply screen -- fires on OK, opens the keyboard for
// the next message so the chat flows without a trip back to the menu.
static void nb_ask_again_cb(GuiButtonType result, InputType type, void* context) {
    UNUSED(result);
    if(type != InputTypeShort) return;
    nb_open_text_input((NikitaBuddy*)context);
}

static void nb_begin_wait(NikitaBuddy* app) {
    nb_show_result(app);
    nb_switch(app, NbViewResult);
    furi_timer_start(app->poll_timer, furi_ms_to_ticks(NB_POLL_MS));
}

// ---- Callbacks -----------------------------------------------------------

static void nb_poll_timer_cb(void* context) {
    NikitaBuddy* app = context;
    // Do the file read and UI work on the GUI thread, not here.
    view_dispatcher_send_custom_event(app->view_dispatcher, NbCustomPoll);
}

static bool nb_custom_event_cb(void* context, uint32_t event) {
    NikitaBuddy* app = context;
    if(event != NbCustomPoll) return false;
    if(app->answered) return true;

    app->waited_s += NB_POLL_MS / 1000;

    char buf[NB_REPLY_MAX];
    if(nb_read_file(app, NB_RES_PATH, buf, sizeof(buf)) > 0) {
        uint32_t id = 0;
        if(nb_json_uint(buf, "\"id\"", &id) && id == app->req_id) {
            if(nb_json_text(buf, app->reply, sizeof(app->reply))) {
                app->answered = true;
                furi_timer_stop(app->poll_timer);
                notification_message(app->notifications, &sequence_success);
                nb_show_result(app); // draw the reply ONCE, then leave it be
                return true;
            }
        }
    }
    // Only redraw when crossing into the "no answer yet" hint -- otherwise the
    // waiting screen is static, so rebuilding it every second (which resets the
    // text-scroll position) is avoided and the user can actually scroll.
    if(!app->hint_shown && app->waited_s >= NB_HINT_AFTER_S) {
        app->hint_shown = true;
        nb_show_result(app);
    }
    return true;
}

static void nb_text_done_cb(void* context) {
    NikitaBuddy* app = context;
    if(strlen(app->prompt) == 0) {
        nb_switch(app, NbViewMenu);
        return;
    }
    nb_send_prompt(app, app->prompt);
    nb_begin_wait(app);
}

static void nb_menu_cb(void* context, uint32_t index) {
    NikitaBuddy* app = context;
    // A quick command: indices NbMenuQuick1 .. NbMenuQuick1 + quick_count - 1.
    if(index >= NbMenuQuick1 && index < NbMenuQuick1 + app->quick_count) {
        size_t q = index - NbMenuQuick1;
        nb_send_prompt(app, app->quick_prompts[q]);
        nb_begin_wait(app);
        return;
    }
    switch(index) {
    case NbMenuType:
        app->prompt[0] = '\0';
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, "Ask Nikita");
        text_input_set_result_callback(
            app->text_input, nb_text_done_cb, app, app->prompt, sizeof(app->prompt), true);
        nb_switch(app, NbViewText);
        break;
    case NbMenuAbout:
        widget_reset(app->widget);
        app->answered = true; // static screen, no polling
        widget_add_text_scroll_element(
            app->widget,
            0,
            0,
            128,
            64,
            "Nikita Buddy v0.1\n\n"
            "Ask Nikita from the Flipper.\nYour question goes to the SD\n"
            "card mailbox; the iPhone or\nqFlipper answers over its\n"
            "Kimi link. No internet on the\nFlipper itself.\n\n"
            "/ext/nikita/buddy/");
        nb_switch(app, NbViewResult);
        break;
    default:
        break;
    }
}

// Back: from a sub-view return to the menu; from the menu, leave the app.
static bool nb_back_event_cb(void* context) {
    NikitaBuddy* app = context;
    if(app->current_view == NbViewMenu) {
        return false; // exit
    }
    furi_timer_stop(app->poll_timer);
    nb_switch(app, NbViewMenu);
    return true;
}

// ---- Menu build ----------------------------------------------------------

static void nb_build_menu(NikitaBuddy* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Nikita Buddy");
    submenu_add_item(app->submenu, "Type a message", NbMenuType, nb_menu_cb, app);
    for(size_t i = 0; i < app->quick_count; i++) {
        submenu_add_item(
            app->submenu, app->quick_labels[i], NbMenuQuick1 + i, nb_menu_cb, app);
    }
    submenu_add_item(app->submenu, "About", NbMenuAbout, nb_menu_cb, app);
}

// ---- Lifecycle -----------------------------------------------------------

static NikitaBuddy* nikita_buddy_alloc(void) {
    NikitaBuddy* app = malloc(sizeof(NikitaBuddy));
    memset(app, 0, sizeof(NikitaBuddy));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->next_id = furi_get_tick(); // unlikely to collide with a stale res.json

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, nb_custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, nb_back_event_cb);

    app->submenu = submenu_alloc();
    nb_load_quick(app);   // pull quick commands synced from phone/qFlipper
    nb_build_menu(app);
    view_dispatcher_add_view(app->view_dispatcher, NbViewMenu, submenu_get_view(app->submenu));

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, NbViewText, text_input_get_view(app->text_input));

    app->widget = widget_alloc();
    view_dispatcher_add_view(app->view_dispatcher, NbViewResult, widget_get_view(app->widget));

    app->poll_timer = furi_timer_alloc(nb_poll_timer_cb, FuriTimerTypePeriodic, app);

    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    return app;
}

static void nikita_buddy_free(NikitaBuddy* app) {
    furi_timer_stop(app->poll_timer);
    furi_timer_free(app->poll_timer);

    view_dispatcher_remove_view(app->view_dispatcher, NbViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, NbViewText);
    view_dispatcher_remove_view(app->view_dispatcher, NbViewResult);
    submenu_free(app->submenu);
    text_input_free(app->text_input);
    widget_free(app->widget);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t nikita_buddy_app(void* p) {
    UNUSED(p);
    NikitaBuddy* app = nikita_buddy_alloc();
    nb_switch(app, NbViewMenu);
    view_dispatcher_run(app->view_dispatcher);
    nikita_buddy_free(app);
    return 0;
}
