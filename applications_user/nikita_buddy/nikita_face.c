#include "nikita_face.h"

#include <gui/canvas.h>
#include <input/input.h>
#include <string.h>

// Screen is 128x64, 1-bit. We fill the whole thing black and draw the face in
// WHITE -- on the Flipper an "off" pixel shows the orange backlight, so white
// strokes read as a lit face glowing on a dark field, exactly like the mockup.

#define FACE_TEXT_MAX 2048

typedef struct {
    NikitaFaceMood mood;
    uint32_t frame; // animation counter, advanced by the app's timer
    char text[FACE_TEXT_MAX];
    size_t shown; // characters of `text` revealed so far (typewriter)
    size_t len;
    void (*ok_cb)(void* ctx);
    void* ok_ctx;
} FaceModel;

// A 0..amp..0 triangle wave over a period, integer math. Gives the grin its
// interlocking teeth and, reused, the eyelids' slant.
static int tri_wave(int x, int period, int amp) {
    int m = ((x % period) + period) % period;
    int half = period / 2;
    int t = (m < half) ? m : (period - m);
    // scale t (0..half) to 0..amp
    return (half > 0) ? (t * amp) / half : 0;
}

// ---- the face -----------------------------------------------------------

// Eyes: two angry, angled almonds -- inner edge tall and vertical, outer edge
// slanting down. `open` (0..100) scales their height so she can blink and
// narrow them when thinking.
static void draw_eyes(Canvas* canvas, int open) {
    const int top_max = 6, bottom = 18, span = bottom - top_max;
    int top = bottom - (span * open) / 100;
    if(top < top_max) top = top_max;
    if(top > bottom) top = bottom;

    for(int y = top; y <= bottom; y++) {
        int prog = ((y - top_max) * 100) / (span == 0 ? 1 : span); // 0..100
        // Left eye: fill from left(y)..58, left edge slants in as y grows.
        int left = 58 - (24 * prog) / 100;
        if(left < 34) left = 34;
        canvas_draw_line(canvas, left, y, 58, y);
        // Right eye: mirror, fill 70..right(y).
        int right = 70 + (24 * prog) / 100;
        if(right > 94) right = 94;
        canvas_draw_line(canvas, 70, y, right, y);
    }
}

// Mouth: the classic pumpkin zig-zag grin. A filled band whose top and bottom
// edges are interlocking triangle waves (teeth). `height` opens it -- small is
// a closed jagged grin, large is mid-word.
static void draw_mouth(Canvas* canvas, int height) {
    const int base = 21;
    for(int x = 34; x <= 94; x++) {
        int t = tri_wave(x - 34, 9, 4); // 0..4 teeth
        int top = base + (4 - t); // upper teeth point down
        int bot = base + height + t; // lower teeth point up
        if(bot > 32) bot = 32;
        if(bot < top) bot = top;
        canvas_draw_line(canvas, x, top, x, bot);
    }
}

// The face for the current mood/frame.
static void draw_face(Canvas* canvas, const FaceModel* m) {
    // Blink roughly every ~2.2s (a couple of frames closed).
    bool blink = (m->frame % 22) >= 20;
    int open, height;
    bool talking = (m->mood == NikitaFaceTalking) && (m->shown < m->len);

    switch(m->mood) {
    case NikitaFaceThinking:
        open = blink ? 8 : 42; // narrowed, pensive
        height = 3;
        break;
    case NikitaFaceTalking:
        open = blink ? 10 : 92;
        if(talking) {
            static const int talk[4] = {2, 6, 10, 5};
            height = talk[m->frame % 4]; // mouth works while typing
        } else {
            height = 4; // done speaking: settle to a grin
        }
        break;
    default: // Idle
        open = blink ? 8 : 100;
        height = 4;
        break;
    }
    draw_eyes(canvas, open);
    draw_mouth(canvas, height);

    // Thinking shows a pulsing "..." under the grin.
    if(m->mood == NikitaFaceThinking) {
        int dots = (m->frame / 4) % 4; // 0..3
        for(int i = 0; i < dots; i++) {
            canvas_draw_disc(canvas, 58 + i * 6, 30, 1);
        }
    }
}

// ---- Undertale-style text box ------------------------------------------

// Draw the revealed text, word-wrapped, last 3 lines, inside a bordered box.
static void draw_textbox(Canvas* canvas, const FaceModel* m) {
    const int bx = 1, by = 34, bw = 126, bh = 29;
    canvas_draw_rframe(canvas, bx, by, bw, bh, 3); // white rounded border
    canvas_set_font(canvas, FontSecondary);

    const int inner_left = bx + 4;
    const int max_w = bw - 8;

    // Wrap the shown substring into lines, keep only the last 3.
    char shown[FACE_TEXT_MAX];
    size_t n = m->shown;
    if(n >= sizeof(shown)) n = sizeof(shown) - 1;
    memcpy(shown, m->text, n);
    shown[n] = '\0';

    char lines[3][64];
    int line_count = 0;
    char cur[64];
    cur[0] = '\0';

    // Walk the shown text word by word (strtok_r isn't in the Flipper API).
    // Spaces and newlines both separate words; the box is small, so wrapping by
    // width is enough. Keep only the last 3 lines (a rolling window).
    const char* p = shown;
    while(*p) {
        while(*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if(!*p) break;
        const char* start = p;
        while(*p && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
        size_t wl = (size_t)(p - start);
        char w[64];
        if(wl >= sizeof(w)) wl = sizeof(w) - 1;
        memcpy(w, start, wl);
        w[wl] = '\0';

        char trial[130];
        if(cur[0] == '\0') {
            strlcpy(trial, w, sizeof(trial));
        } else {
            strlcpy(trial, cur, sizeof(trial));
            strlcat(trial, " ", sizeof(trial));
            strlcat(trial, w, sizeof(trial));
        }
        if((int)canvas_string_width(canvas, trial) <= max_w) {
            strlcpy(cur, trial, sizeof(cur));
        } else {
            if(line_count < 3) {
                strlcpy(lines[line_count++], cur, sizeof(lines[0]));
            } else {
                strlcpy(lines[0], lines[1], sizeof(lines[0]));
                strlcpy(lines[1], lines[2], sizeof(lines[1]));
                strlcpy(lines[2], cur, sizeof(lines[2]));
            }
            strlcpy(cur, w, sizeof(cur));
        }
    }
    if(cur[0] != '\0') {
        if(line_count < 3) {
            strlcpy(lines[line_count++], cur, sizeof(lines[0]));
        } else {
            strlcpy(lines[0], lines[1], sizeof(lines[0]));
            strlcpy(lines[1], lines[2], sizeof(lines[1]));
            strlcpy(lines[2], cur, sizeof(lines[2]));
        }
    }

    for(int i = 0; i < line_count; i++) {
        canvas_draw_str(canvas, inner_left, by + 9 + i * 8, lines[i]);
    }
}

// ---- View plumbing ------------------------------------------------------

static void face_draw_callback(Canvas* canvas, void* model) {
    FaceModel* m = model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 64);
    canvas_set_color(canvas, ColorWhite);
    draw_face(canvas, m);
    draw_textbox(canvas, m);
}

static bool face_input_callback(InputEvent* event, void* context) {
    View* view = context;
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        void (*cb)(void*) = NULL;
        void* ctx = NULL;
        with_view_model(
            view, FaceModel * m, {
                cb = m->ok_cb;
                ctx = m->ok_ctx;
            }, false);
        if(cb) {
            cb(ctx);
            return true;
        }
    }
    return false; // let Back reach the view dispatcher's navigation callback
}

View* nikita_face_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(FaceModel));
    with_view_model(
        view, FaceModel * m, {
            m->mood = NikitaFaceThinking;
            m->frame = 0;
            m->text[0] = '\0';
            m->shown = 0;
            m->len = 0;
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
            if(mood == NikitaFaceTalking) m->frame = 0;
        }, true);
}

void nikita_face_set_text(View* view, const char* text) {
    with_view_model(
        view, FaceModel * m, {
            strlcpy(m->text, text ? text : "", sizeof(m->text));
            m->len = strlen(m->text);
            m->shown = 0;
        }, true);
}

bool nikita_face_tick(View* view) {
    bool more = false;
    with_view_model(
        view, FaceModel * m, {
            m->frame++;
            if(m->mood == NikitaFaceTalking && m->shown < m->len) {
                m->shown += 2; // ~20 chars/sec at a 10fps tick
                if(m->shown > m->len) m->shown = m->len;
            }
            more = (m->shown < m->len);
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
