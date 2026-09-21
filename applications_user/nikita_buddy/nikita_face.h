/*
 * Nikita's face -- her living form on the Flipper's own screen.
 *
 * An animated 1-bit jack-o'-lantern: angled eyes and a zig-zag grin, drawn in
 * the Flipper's default look (the whole screen filled, the face glowing through
 * it -- pixels OFF show the orange backlight, so she reads as a lit face on a
 * dark field). She moves: she blinks, her eyes narrow when she thinks, and her
 * mouth works frame by frame while she talks. Below her, the conversation shows
 * Undertale-style -- a bordered box with the words typing themselves out.
 *
 * This is a plain View, driven by the app: set the mood, hand it text, and tick
 * it from a timer. It owns no timer and no storage of its own.
 */
#pragma once

#include <gui/view.h>

typedef enum {
    NikitaFaceIdle,      // resting: calm eyes, closed grin, the odd blink
    NikitaFaceThinking,  // waiting on the answer: eyes narrowed, a pulsing "..."
    NikitaFaceTalking,   // delivering the reply: mouth working, text typing out
} NikitaFaceMood;

// Create/destroy the face view.
View* nikita_face_alloc(void);
void nikita_face_free(View* view);

// Set her mood. Talking restarts the mouth cycle; the app sets the text too.
void nikita_face_set_mood(View* view, NikitaFaceMood mood);

// Hand her a line to speak. Resets the typewriter to the start.
void nikita_face_set_text(View* view, const char* text);

// Advance one animation step (blink, mouth, "...", one more typed char).
// Call from a periodic timer (~10-12 fps feels alive). Returns true while the
// typewriter still has characters left to reveal.
bool nikita_face_tick(View* view);

// OK on the face view calls this (e.g. to ask again); Back is handled by the
// view dispatcher's navigation callback as usual.
void nikita_face_set_ok_callback(View* view, void (*cb)(void* ctx), void* ctx);
