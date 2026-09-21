#include "nikita_face.h"
#include "nikita_face_frames.h"

#include <gui/canvas.h>
#include <input/input.h>
#include <string.h>

// The face is the user's own reference art, traced to 128x64 1-bit (see
// nikita_face_frames.h). Screen filled black, face drawn WHITE so an "off"
// pixel shows the orange backlight -- a bright orange face on a dark field.
//
// Her reply is shown like a video-game dialogue box: the full answer is wrapped
// into pages of two lines; each page types itself out, and you press OK/Down to
// advance (a blinking v marks "there's more"), Up to go back. Nothing is cut --
// long answers just span more pages.

#define FACE_COLS 20        // chars per line at FontSecondary within the box
#define FACE_MAX_LINES 48   // up to ~960 chars of reply, paged 2 lines at a time
#define FACE_PAGE_LINES 2

typedef struct {
    NikitaFaceMood mood;
    uint32_t frame; // animation counter, advanced by the app's timer

    char lines[FACE_MAX_LINES][FACE_COLS + 2];
    int line_count;
    int page;       // current page index (page p = lines p*2, p*2+1)
    int shown;      // chars revealed on the current page (typewriter)
    int page_chars; // total chars on the current page

    void (*ok_cb)(void* ctx);
    void* ok_ctx;
} FaceModel;

// ---- paging helpers -------------------------------------------------------

static int face_page_first_line(const FaceModel* m) {
    return m->page * FACE_PAGE_LINES;
}
static bool face_more_pages(const FaceModel* m) {
    return (m->page + 1) * FACE_PAGE_LINES < m->line_count;
}
static int face_compute_page_chars(const FaceModel* m) {
    int total = 0;
    int base = face_page_first_line(m);
    for(int i = 0; i < FACE_PAGE_LINES; i++) {
        int li = base + i;
        if(li < m->line_count) total += (int)strlen(m->lines[li]);
    }
    return total;
}

// Wrap `text` into fixed-width lines by a character budget, at word boundaries.
// Newlines force a line break. No canvas measuring (this must stay cheap and
// off the GUI-thread stack concerns) -- fixed 20-col budget.
static void face_wrap(FaceModel* m, const char* text) {
    m->line_count = 0;
    int col = 0;
    m->lines[0][0] = '\0';
    const char* p = text ? text : "";
    while(*p && m->line_count < FACE_MAX_LINES) {
        // explicit newline
        if(*p == '\n' || *p == '\r') {
            p++;
            if(col > 0 || m->line_count == 0) {
                m->lines[m->line_count][col] = '\0';
                m->line_count++;
                if(m->line_count < FACE_MAX_LINES) { col = 0; m->lines[m->line_count][0] = '\0'; }
            }
            continue;
        }
        if(*p == ' ' || *p == '\t') {
            // start-of-line spaces collapse; otherwise a pending space
            if(col > 0 && col < FACE_COLS) m->lines[m->line_count][col++] = ' ';
            p++;
            continue;
        }
        // measure the word
        const char* ws = p;
        while(*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        int wl = (int)(p - ws);
        if(wl > FACE_COLS) wl = FACE_COLS; // clamp a giant token
        // wrap if it doesn't fit (drop a trailing space we may have added)
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
    m->page = 0;
    m->shown = 0;
    m->page_chars = face_compute_page_chars(m);
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

    // Thinking with nothing to say yet: a pulsing "..."
    if(m->line_count == 0) {
        int dots = (m->frame / 4) % 4;
        char d[5] = {0};
        for(int i = 0; i < dots; i++) d[i] = '.';
        canvas_draw_str(canvas, pad_x, by + 13, d);
        return;
    }

    // The current page's two lines, revealed up to `shown` chars.
    int base = face_page_first_line(m);
    for(int i = 0; i < FACE_PAGE_LINES; i++) {
        int li = base + i;
        if(li >= m->line_count) break;
        int len = (int)strlen(m->lines[li]);
        // how many chars of THIS line are revealed
        int before = 0;
        for(int j = 0; j < i; j++) {
            int lj = base + j;
            if(lj < m->line_count) before += (int)strlen(m->lines[lj]);
        }
        int rev = m->shown - before;
        if(rev < 0) rev = 0;
        if(rev > len) rev = len;
        char buf[FACE_COLS + 2];
        memcpy(buf, m->lines[li], rev);
        buf[rev] = '\0';
        canvas_draw_str(canvas, pad_x, by + 9 + i * 8, buf);
    }

    // "more" arrow once the page is fully typed and there's another page.
    bool page_done = m->shown >= m->page_chars;
    if(page_done && face_more_pages(m) && ((m->frame / 4) % 2) == 0) {
        canvas_draw_str(canvas, 120, by + bh - 3, "v");
    }
}

static void face_draw_callback(Canvas* canvas, void* model) {
    FaceModel* m = model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 64);
    canvas_set_color(canvas, ColorWhite);

    bool blink = (m->frame % 22) >= 20;
    if(m->mood == NikitaFaceThinking) blink = (m->frame % 8) >= 5;
    bool talking = (m->mood == NikitaFaceTalking);
    bool typing = talking && (m->shown < m->page_chars);
    bool mouth_open = typing && (((m->frame / 2) % 2) == 0);

    canvas_draw_xbm(canvas, 0, 0, NIKITA_FACE_W, NIKITA_FACE_H, pick_frame(blink, mouth_open));

    if(m->line_count > 0 || m->mood == NikitaFaceThinking) draw_textbox(canvas, m);
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
            if(m->line_count == 0) {
                // No dialogue (intro / thinking): OK falls through to the app.
                if(event->key == InputKeyOk) {
                    cb = m->ok_cb;
                    ctx = m->ok_ctx;
                }
            } else if(event->key == InputKeyOk || event->key == InputKeyDown) {
                if(m->shown < m->page_chars) {
                    m->shown = m->page_chars; // fast-forward the current page
                    handled = true;
                } else if(face_more_pages(m)) {
                    m->page++; // next page of dialogue
                    m->shown = 0;
                    m->page_chars = face_compute_page_chars(m);
                    handled = true;
                } else if(event->key == InputKeyOk) {
                    // Last page, fully read: OK asks again / leaves.
                    cb = m->ok_cb;
                    ctx = m->ok_ctx;
                }
            } else if(event->key == InputKeyUp) {
                if(m->page > 0) {
                    m->page--; // back a page, shown in full
                    m->page_chars = face_compute_page_chars(m);
                    m->shown = m->page_chars;
                    handled = true;
                }
            }
        },
        true);

    if(cb) {
        cb(ctx);
        return true;
    }
    return handled; // Back stays with the view dispatcher's navigation callback
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
            m->page = 0;
            m->shown = 0;
            m->page_chars = 0;
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
                m->page = 0;
                m->shown = 0;
                m->page_chars = face_compute_page_chars(m);
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
            if(m->mood == NikitaFaceTalking && m->shown < m->page_chars) {
                m->shown += 2; // ~20 chars/sec at a 10fps tick
                if(m->shown > m->page_chars) m->shown = m->page_chars;
            }
            more = (m->shown < m->page_chars) || face_more_pages(m);
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
