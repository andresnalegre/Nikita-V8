#include "nikita_face.h"
#include "nikita_face_frames.h"

#include <gui/canvas.h>
#include <input/input.h>
#include <string.h>

// The face is the user's own reference art, traced to 128x64 1-bit (see
// nikita_face_frames.h). We fill the screen black and draw the face in WHITE --
// on the Flipper an "off" pixel shows the orange backlight, so she reads as a
// bright orange face on a dark field, exactly like the reference. She is alive:
// her eyes blink and, while she talks, her jaw works; the reply types itself
// into a box across the bottom, over the whole face (never distorting it).

#define FACE_TEXT_MAX 2048

typedef struct {
    NikitaFaceMood mood;
    uint32_t frame; // animation counter, advanced by the app's timer
    char text[FACE_TEXT_MAX];
    size_t shown; // characters revealed so far (typewriter)
    size_t len;
    void (*ok_cb)(void* ctx);
    void* ok_ctx;
} FaceModel;

// Choose the right traced frame for the current eye/mouth state.
static const unsigned char* pick_frame(bool eyes_shut, bool mouth_open) {
    if(eyes_shut && mouth_open) return NIKITA_FACE_TALK_BLINK;
    if(eyes_shut) return NIKITA_FACE_BLINK;
    if(mouth_open) return NIKITA_FACE_TALK;
    return NIKITA_FACE_IDLE;
}

// The Undertale-style box, laid OVER the bottom of the face so the eyes and the
// working jaw stay visible above it. Two lines, typed out.
static void draw_textbox(Canvas* canvas, const FaceModel* m) {
    // The box lives in the empty space BELOW the face, never over it -- the
    // whole face stays visible and uncut. Frame only (no black fill needed,
    // since there's nothing to cover there).
    const int by = NIKITA_FACE_BOTTOM + 2;
    const int bh = 64 - by;
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_rframe(canvas, 0, by, 128, bh, 2);
    canvas_set_font(canvas, FontSecondary);

    // Generous inner padding so text never touches the border.
    const int pad_x = 6;
    const int max_w = 128 - pad_x * 2 - 4;
    char shown[FACE_TEXT_MAX];
    size_t n = m->shown;
    if(n >= sizeof(shown)) n = sizeof(shown) - 1;
    memcpy(shown, m->text, n);
    shown[n] = '\0';

    // Wrap into lines, keep the last 2 (rolling window) so the newest text shows.
    char lines[2][64];
    int line_count = 0;
    char cur[64];
    cur[0] = '\0';
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
            if(line_count < 2) {
                strlcpy(lines[line_count++], cur, sizeof(lines[0]));
            } else {
                strlcpy(lines[0], lines[1], sizeof(lines[0]));
                strlcpy(lines[1], cur, sizeof(lines[1]));
            }
            strlcpy(cur, w, sizeof(cur));
        }
    }
    if(cur[0] != '\0') {
        if(line_count < 2) {
            strlcpy(lines[line_count++], cur, sizeof(lines[0]));
        } else {
            strlcpy(lines[0], lines[1], sizeof(lines[0]));
            strlcpy(lines[1], cur, sizeof(lines[1]));
        }
    }
    for(int i = 0; i < line_count; i++) {
        canvas_draw_str(canvas, pad_x, by + 9 + i * 8, lines[i]);
    }
}

static void face_draw_callback(Canvas* canvas, void* model) {
    FaceModel* m = model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 64);
    canvas_set_color(canvas, ColorWhite);

    // Blink every ~2.2s for a couple of frames.
    bool blink = (m->frame % 22) >= 20;
    // Thinking narrows her gaze (a slow blink cadence); talking works the jaw.
    if(m->mood == NikitaFaceThinking) blink = (m->frame % 8) >= 5;
    bool talking = (m->mood == NikitaFaceTalking);
    bool mouth_open = talking && (((m->frame / 2) % 2) == 0);

    canvas_draw_xbm(canvas, 0, 0, NIKITA_FACE_W, NIKITA_FACE_H, pick_frame(blink, mouth_open));

    // Her words, over the bottom, only once there's something to say.
    if(m->len > 0) draw_textbox(canvas, m);
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
