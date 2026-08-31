#include "hid_parasite.h"
#include <gui/elements.h>
#include "../hid.h"

#include "hid_icons.h"

#define TAG "HidParasite"

// PARASITE -- the Flipper plugs into the host and drives it from the inside. The
// D-pad walks a file explorer on the host over USB HID; the host's own monitor
// is the return channel -- you read the result with your eyes, not over USB.
// Nikita's Remote Control (iOS) injects the same InputEvents this view receives,
// so the phone drives it exactly as the physical D-pad does.
//
// OK is Enter (open / go into), Back is Backspace (up / back), the arrows are
// the arrow keys. Hold Back to detach.
struct HidParasite {
    View* view;
    Hid* hid;
};

typedef struct {
    bool left_pressed;
    bool up_pressed;
    bool right_pressed;
    bool down_pressed;
    bool ok_pressed;
    bool back_pressed;
    bool connected;
} HidParasiteModel;

static void hid_parasite_draw_arrow(Canvas* canvas, uint8_t x, uint8_t y, CanvasDirection dir) {
    canvas_draw_triangle(canvas, x, y, 5, 3, dir);
    if(dir == CanvasDirectionBottomToTop) {
        canvas_draw_line(canvas, x, y + 6, x, y - 1);
    } else if(dir == CanvasDirectionTopToBottom) {
        canvas_draw_line(canvas, x, y - 6, x, y + 1);
    } else if(dir == CanvasDirectionRightToLeft) {
        canvas_draw_line(canvas, x + 6, y, x - 1, y);
    } else if(dir == CanvasDirectionLeftToRight) {
        canvas_draw_line(canvas, x - 6, y, x + 1, y);
    }
}

static void hid_parasite_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    HidParasiteModel* model = context;

    // Header
#ifdef HID_TRANSPORT_BLE
    if(model->connected) {
        canvas_draw_icon(canvas, 0, 0, &I_Ble_connected_15x15);
    } else {
        canvas_draw_icon(canvas, 0, 0, &I_Ble_disconnected_15x15);
    }
#endif

    canvas_set_font(canvas, FontPrimary);
    elements_multiline_text_aligned(canvas, 17, 3, AlignLeft, AlignTop, "PARASITE");

    canvas_draw_icon(canvas, 68, 2, &I_Pin_back_arrow_10x8);
    canvas_set_font(canvas, FontSecondary);
    elements_multiline_text_aligned(canvas, 127, 3, AlignRight, AlignTop, "Hold to exit");

    // Up
    canvas_draw_icon(canvas, 21, 24, &I_Button_18x18);
    if(model->up_pressed) {
        elements_slightly_rounded_box(canvas, 24, 26, 13, 13);
        canvas_set_color(canvas, ColorWhite);
    }
    hid_parasite_draw_arrow(canvas, 30, 30, CanvasDirectionBottomToTop);
    canvas_set_color(canvas, ColorBlack);

    // Down
    canvas_draw_icon(canvas, 21, 45, &I_Button_18x18);
    if(model->down_pressed) {
        elements_slightly_rounded_box(canvas, 24, 47, 13, 13);
        canvas_set_color(canvas, ColorWhite);
    }
    hid_parasite_draw_arrow(canvas, 30, 55, CanvasDirectionTopToBottom);
    canvas_set_color(canvas, ColorBlack);

    // Left
    canvas_draw_icon(canvas, 0, 45, &I_Button_18x18);
    if(model->left_pressed) {
        elements_slightly_rounded_box(canvas, 3, 47, 13, 13);
        canvas_set_color(canvas, ColorWhite);
    }
    hid_parasite_draw_arrow(canvas, 7, 53, CanvasDirectionRightToLeft);
    canvas_set_color(canvas, ColorBlack);

    // Right
    canvas_draw_icon(canvas, 42, 45, &I_Button_18x18);
    if(model->right_pressed) {
        elements_slightly_rounded_box(canvas, 45, 47, 13, 13);
        canvas_set_color(canvas, ColorWhite);
    }
    hid_parasite_draw_arrow(canvas, 53, 53, CanvasDirectionLeftToRight);
    canvas_set_color(canvas, ColorBlack);

    // Ok -> Enter (open / go into)
    canvas_draw_icon(canvas, 63, 24, &I_Space_65x18);
    if(model->ok_pressed) {
        elements_slightly_rounded_box(canvas, 66, 26, 60, 13);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_icon(canvas, 74, 28, &I_Ok_btn_9x9);
    elements_multiline_text_aligned(canvas, 91, 36, AlignLeft, AlignBottom, "Open");
    canvas_set_color(canvas, ColorBlack);

    // Back -> Backspace (up / back)
    canvas_draw_icon(canvas, 63, 45, &I_Space_65x18);
    if(model->back_pressed) {
        elements_slightly_rounded_box(canvas, 66, 47, 60, 13);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_icon(canvas, 74, 49, &I_Pin_back_arrow_10x8);
    elements_multiline_text_aligned(canvas, 91, 57, AlignLeft, AlignBottom, "Up dir");
}

static void hid_parasite_process(HidParasite* hid_parasite, InputEvent* event) {
    with_view_model(
        hid_parasite->view,
        HidParasiteModel * model,
        {
            if(event->type == InputTypePress) {
                if(event->key == InputKeyUp) {
                    model->up_pressed = true;
                    hid_hal_keyboard_press(hid_parasite->hid, HID_KEYBOARD_UP_ARROW);
                } else if(event->key == InputKeyDown) {
                    model->down_pressed = true;
                    hid_hal_keyboard_press(hid_parasite->hid, HID_KEYBOARD_DOWN_ARROW);
                } else if(event->key == InputKeyLeft) {
                    model->left_pressed = true;
                    hid_hal_keyboard_press(hid_parasite->hid, HID_KEYBOARD_LEFT_ARROW);
                } else if(event->key == InputKeyRight) {
                    model->right_pressed = true;
                    hid_hal_keyboard_press(hid_parasite->hid, HID_KEYBOARD_RIGHT_ARROW);
                } else if(event->key == InputKeyOk) {
                    model->ok_pressed = true;
                    hid_hal_keyboard_press(hid_parasite->hid, HID_KEYBOARD_RETURN);
                } else if(event->key == InputKeyBack) {
                    model->back_pressed = true;
                }
            } else if(event->type == InputTypeRelease) {
                if(event->key == InputKeyUp) {
                    model->up_pressed = false;
                    hid_hal_keyboard_release(hid_parasite->hid, HID_KEYBOARD_UP_ARROW);
                } else if(event->key == InputKeyDown) {
                    model->down_pressed = false;
                    hid_hal_keyboard_release(hid_parasite->hid, HID_KEYBOARD_DOWN_ARROW);
                } else if(event->key == InputKeyLeft) {
                    model->left_pressed = false;
                    hid_hal_keyboard_release(hid_parasite->hid, HID_KEYBOARD_LEFT_ARROW);
                } else if(event->key == InputKeyRight) {
                    model->right_pressed = false;
                    hid_hal_keyboard_release(hid_parasite->hid, HID_KEYBOARD_RIGHT_ARROW);
                } else if(event->key == InputKeyOk) {
                    model->ok_pressed = false;
                    hid_hal_keyboard_release(hid_parasite->hid, HID_KEYBOARD_RETURN);
                } else if(event->key == InputKeyBack) {
                    model->back_pressed = false;
                }
            } else if(event->type == InputTypeShort) {
                // Backspace = up one folder in Explorer / back in Finder columns.
                if(event->key == InputKeyBack) {
                    hid_hal_keyboard_press(hid_parasite->hid, HID_KEYBOARD_DELETE);
                    hid_hal_keyboard_release(hid_parasite->hid, HID_KEYBOARD_DELETE);
                }
            }
        },
        true);
}

static bool hid_parasite_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    HidParasite* hid_parasite = context;
    bool consumed = false;

    if(event->type == InputTypeLong && event->key == InputKeyBack) {
        hid_hal_keyboard_release_all(hid_parasite->hid);
    } else {
        hid_parasite_process(hid_parasite, event);
        consumed = true;
    }

    return consumed;
}

HidParasite* hid_parasite_alloc(Hid* hid) {
    HidParasite* hid_parasite = malloc(sizeof(HidParasite));
    hid_parasite->view = view_alloc();
    hid_parasite->hid = hid;
    view_set_context(hid_parasite->view, hid_parasite);
    view_allocate_model(hid_parasite->view, ViewModelTypeLocking, sizeof(HidParasiteModel));
    view_set_draw_callback(hid_parasite->view, hid_parasite_draw_callback);
    view_set_input_callback(hid_parasite->view, hid_parasite_input_callback);
    return hid_parasite;
}

void hid_parasite_free(HidParasite* hid_parasite) {
    furi_assert(hid_parasite);
    view_free(hid_parasite->view);
    free(hid_parasite);
}

View* hid_parasite_get_view(HidParasite* hid_parasite) {
    furi_assert(hid_parasite);
    return hid_parasite->view;
}

void hid_parasite_set_connected_status(HidParasite* hid_parasite, bool connected) {
    furi_assert(hid_parasite);
    with_view_model(
        hid_parasite->view, HidParasiteModel * model, { model->connected = connected; }, true);
}
