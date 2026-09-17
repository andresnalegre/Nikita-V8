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

#define NB_DIR "/ext/nikita"
#define NB_BUDDY_DIR "/ext/nikita/buddy"
#define NB_REQ_PATH "/ext/nikita/buddy/req.json"
#define NB_RES_PATH "/ext/nikita/buddy/res.json"

#define NB_PROMPT_MAX 512
#define NB_REPLY_MAX 2048
#define NB_POLL_MS 1000
// After this many seconds with no reply, tell the user what is missing.
#define NB_HINT_AFTER_S 8

typedef enum {
    NbViewMenu,
    NbViewText,
    NbViewResult,
} NbView;

typedef enum {
    NbMenuType, // free text
    NbMenuQuick1,
    NbMenuQuick2,
    NbMenuQuick3,
    NbMenuAbout,
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
    NbView current_view; // tracked ourselves; the API has no getter
} NikitaBuddy;

// Switch view and remember where we are, so Back knows menu-vs-subview.
static void nb_switch(NikitaBuddy* app, NbView view);

// ---- Quick commands ------------------------------------------------------
// Prompts you can send without typing. Kept short and genuinely useful on a
// device with a D-pad for a keyboard.
static const char* const nb_quick_labels[] = {
    "Resumir a conversa",
    "O que voce lembra de mim?",
    "Status do meu Flipper",
};
static const char* const nb_quick_prompts[] = {
    "Resuma em poucas linhas o que conversamos ultimamente.",
    "O que voce lembra sobre mim? Liste em topicos curtos.",
    "Me de um resumo do estado do meu Flipper: firmware, espaco no SD e apps instalados.",
};

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

// Drop the request in the mailbox and clear any stale reply so a leftover
// res.json from a previous question cannot be mistaken for this one's answer.
static void nb_send_prompt(NikitaBuddy* app, const char* text) {
    strlcpy(app->prompt, text, sizeof(app->prompt));
    app->req_id = ++app->next_id;
    app->waited_s = 0;
    app->answered = false;
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
        furi_string_cat_str(body, app->reply);
    } else {
        furi_string_cat_str(body, "Perguntei ao Nikita...\n\n\"");
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
                "Ainda sem resposta.\nIsto precisa do iPhone ou do qFlipper\n"
                "conectado e com o Nikita ligado.\nDeixei a pergunta salva; ele\n"
                "responde assim que conectar.");
        } else {
            furi_string_cat_printf(body, "Aguardando... (%lus)", (unsigned long)app->waited_s);
        }
    }

    widget_add_text_scroll_element(
        app->widget, 0, 0, 128, 64, furi_string_get_cstr(body));
    furi_string_free(body);
}

static void nb_switch(NikitaBuddy* app, NbView view) {
    app->current_view = view;
    view_dispatcher_switch_to_view(app->view_dispatcher, view);
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
            }
        }
    }
    nb_show_result(app);
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
    switch(index) {
    case NbMenuType:
        app->prompt[0] = '\0';
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, "Ask Nikita");
        text_input_set_result_callback(
            app->text_input, nb_text_done_cb, app, app->prompt, sizeof(app->prompt), true);
        nb_switch(app, NbViewText);
        break;
    case NbMenuQuick1:
    case NbMenuQuick2:
    case NbMenuQuick3: {
        size_t q = index - NbMenuQuick1;
        nb_send_prompt(app, nb_quick_prompts[q]);
        nb_begin_wait(app);
        break;
    }
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
    submenu_add_item(app->submenu, nb_quick_labels[0], NbMenuQuick1, nb_menu_cb, app);
    submenu_add_item(app->submenu, nb_quick_labels[1], NbMenuQuick2, nb_menu_cb, app);
    submenu_add_item(app->submenu, nb_quick_labels[2], NbMenuQuick3, nb_menu_cb, app);
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
