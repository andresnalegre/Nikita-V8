#include "nikita_face.h"
#include "nikita_face_frames.h"

#include <gui/canvas.h>
#include <input/input.h>
#include <string.h>

// The face is the user's own reference art, traced to 128x64 1-bit (see
// nikita_face_frames.h). Screen filled black, face drawn WHITE so an "off"
// pixel shows the orange backlight -- a bright orange face on a dark field.
//
// The dialogue box is NOT shown until Nikita actually says something. When she
// replies, the box appears and types out; UP/DOWN scroll it when the answer is
// taller than the box; OK/Enter DISMISSES the box, leaving just her face.

#define FACE_COLS 20
#define FACE_MAX_LINES 48
#define FACE_WINDOW 2 // visible text lines in the box

typedef struct {
    NikitaFaceMood mood;
    uint32_t frame;

    char lines[FACE_MAX_LINES][FACE_COLS + 2];
    int line_count;
    int total_chars;
    int shown;   // typewriter: chars revealed across all lines
    int scroll;  // index of the top visible line
    bool dismissed; // box hidden (OK pressed) -- face only

    void (*ok_cb)(void* ctx);
    void* ok_ctx;
} FaceModel;

static int face_max_scroll(const FaceModel* m) {
    return (m->line_count > FACE_WINDOW) ? (m->line_count - FACE_WINDOW) : 0;
}
static bool face_typing(const FaceModel* m) {
    return m->mood == NikitaFaceTalking && m->shown < m->total_chars;
}

// Wrap text into fixed-width lines at word boundaries; newlines force a break.
static void face_wrap(FaceModel* m, const char* text) {
    m->line_count = 0;
    m->total_chars = 0;
    int col = 0;
    m->lines[0][0] = '\0';
    const char* p = text ? text : "";
    while(*p && m->line_count < FACE_MAX_LINES) {
        if(*p == '\n' || *p == '\r') {
            p++;
            m->lines[m->line_count][col] = '\0';
            m->line_count++;
            if(m->line_count < FACE_MAX_LINES) { col = 0; m->lines[m->line_count][0] = '\0'; }
            continue;
        }
        if(*p == ' ' || *p == '\t') {
            if(col > 0 && col < FACE_COLS) m->lines[m->line_count][col++] = ' ';
            p++;
            continue;
        }
        const char* ws = p;
        while(*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        int wl = (int)(p - ws);
        if(wl > FACE_COLS) wl = FACE_COLS;
        if(col + wl > FACE_COLS) {
            if(col > 0 && m->lines[m->line_count][col - 1] == ' ') col--;
            m->lines[m->line_count][col] = '\0';
            m->line_count++;
            if(m->line_count >= FACE_MAX_LINES) break;
            col = 0; m->lines[m->line_count][0] = '\0';
        }
        for(int k = 0; k < wl && col < FACE_COLS; k++) m->lines[m->line_count][col++] = ws[k];
        m->lines[m->line_count][col] = '\0';
    }
    if(m->line_count < FACE_MAX_LINES && (col > 0 || m->line_count == 0)) {
        m->lines[m->line_count][col] = '\0';
        m->line_count++;
    }
    for(int i = 0; i < m->line_count; i++) m->total_chars += (int)strlen(m->lines[i]);
    m->shown = 0;
    m->scroll = 0;
    m->dismissed = false;
}

// ---- drawing --------------------------------------------------------------

static const unsigned char* pick_frame(bool eyes_shut, bool mouth_open) {
    if(eyes_shut && mouth_open) return NIKITA_FACE_TALK_BLINK;
    if(eyes_shut) return NIKITA_FACE_BLINK;
    if(mouth_open) return NIKITA_FACE_TALK;
    return NIKITA_FACE_IDLE;
}

static void draw_textbox(Canvas* canvas, const FaceModel* m) {
    const int by = 42, bh = 22, pad_x = 6;
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, by, 128, bh);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_rframe(canvas, 0, by, 128, bh, 2);
    canvas_set_font(canvas, FontSecondary);

    // Thinking, nothing said yet: a pulsing "..."
    if(m->line_count == 0) {
        int dots = (m->frame / 4) % 4;
        char d[5] = {0};
        for(int i = 0; i < dots; i++) d[i] = '.';
        canvas_draw_str(canvas, pad_x, by + 13, d);
        return;
    }

    // chars revealed before the first visible line
    int before = 0;
    for(int i = 0; i < m->scroll && i < m->line_count; i++) before += (int)strlen(m->lines[i]);

    for(int i = 0; i < FACE_WINDOW; i++) {
        int li = m->scroll + i;
        if(li >= m->line_count) break;
        int len = (int)strlen(m->lines[li]);
        int rev = m->shown - before;
        if(rev < 0) rev = 0;
        if(rev > len) rev = len;
        char buf[FACE_COLS + 2];
        memcpy(buf, m->lines[li], rev);
        buf[rev] = '\0';
        canvas_draw_str(canvas, pad_x, by + 9 + i * 8, buf);
        before += len;
    }

    // scroll hints
    if(m->scroll > 0) canvas_draw_str(canvas, 120, by + 8, "^");
    if(m->scroll < face_max_scroll(m) && ((m->frame / 4) % 2) == 0)
        canvas_draw_str(canvas, 120, by + bh - 3, "v");
}

static void face_draw_callback(Canvas* canvas, void* model) {
    FaceModel* m = model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 64);
    canvas_set_color(canvas, ColorWhite);

    bool blink = (m->frame % 22) >= 20;
    if(m->mood == NikitaFaceThinking) blink = (m->frame % 8) >= 5;
    bool mouth_open = face_typing(m) && (((m->frame / 2) % 2) == 0);

    canvas_draw_xbm(canvas, 0, 0, NIKITA_FACE_W, NIKITA_FACE_H, pick_frame(blink, mouth_open));

    // Box only when there's something to say AND it hasn't been dismissed.
    bool show_box = (!m->dismissed) && (m->line_count > 0 || m->mood == NikitaFaceThinking);
    if(show_box) draw_textbox(canvas, m);
}

// ---- input ----------------------------------------------------------------

static bool face_input_callback(InputEvent* event, void* context) {
    View* view = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    bool handled = false;
    void (*cb)(void*) = NULL;
    void* ctx = NULL;

    with_view_model(
        view, FaceModel * m, {
            bool box = (!m->dismissed) && (m->line_count > 0);
            if(event->key == InputKeyOk) {
                if(box) {
                    m->dismissed = true; // Enter makes the message disappear
                    handled = true;
                } else {
                    cb = m->ok_cb; // no box: let the app act (menu / ask again)
                    ctx = m->ok_ctx;
                }
            } else if(event->key == InputKeyDown) {
                if(box && !face_typing(m) && m->scroll < face_max_scroll(m)) {
                    m->scroll++;
                    handled = true;
                }
            } else if(event->key == InputKeyUp) {
                if(box && !face_typing(m) && m->scroll > 0) {
                    m->scroll--;
                    handled = true;
                }
            }
        },
        true);

    if(cb) {
        cb(ctx);
        return true;
    }
    return handled;
}

// ---- view plumbing --------------------------------------------------------

View* nikita_face_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(FaceModel));
    with_view_model(
        view, FaceModel * m, {
            m->mood = NikitaFaceThinking;
            m->frame = 0;
            m->line_count = 0;
            m->total_chars = 0;
            m->shown = 0;
            m->scroll = 0;
            m->dismissed = false;
            m->ok_cb = NULL;
            m->ok_ctx = NULL;
        }, true);
    view_set_context(view, view);
    view_set_draw_callback(view, face_draw_callback);
    view_set_input_callback(view, face_input_callback);
    return view;
}

void nikita_face_free(View* view) {
    view_free(view);
}

void nikita_face_set_mood(View* view, NikitaFaceMood mood) {
    with_view_model(
        view, FaceModel * m, {
            m->mood = mood;
            if(mood == NikitaFaceTalking) {
                m->frame = 0;
                m->shown = 0;
                m->scroll = 0;
                m->dismissed = false;
            }
        }, true);
}

void nikita_face_set_text(View* view, const char* text) {
    with_view_model(view, FaceModel * m, { face_wrap(m, text); }, true);
}

bool nikita_face_tick(View* view) {
    bool more = false;
    with_view_model(
        view, FaceModel * m, {
            m->frame++;
            if(m->mood == NikitaFaceTalking && m->shown < m->total_chars) {
                m->shown += 2; // ~20 chars/sec at a 10fps tick
                if(m->shown > m->total_chars) m->shown = m->total_chars;
                // auto-follow: keep the line being typed in view
                int cum = 0;
                int cur = 0;
                for(int i = 0; i < m->line_count; i++) {
                    cum += (int)strlen(m->lines[i]);
                    cur = i;
                    if(cum >= m->shown) break;
                }
                int want = cur - (FACE_WINDOW - 1);
                if(want < 0) want = 0;
                m->scroll = want;
                more = true;
            }
        }, true);
    return more;
}

void nikita_face_set_ok_callback(View* view, void (*cb)(void* ctx), void* ctx) {
    with_view_model(
        view, FaceModel * m, {
            m->ok_cb = cb;
            m->ok_ctx = ctx;
        }, false);
}
