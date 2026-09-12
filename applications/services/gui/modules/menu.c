#include "menu.h"

#include <gui/elements.h>
#include <gui/icon_animation.h>
#include <assets_icons.h>
#include <furi.h>
#include <m-array.h>

struct Menu {
    View* view;
};

typedef struct {
    const char* label;
    IconAnimation* icon;
    uint32_t index;
    MenuItemCallback callback;
    void* callback_context;
} MenuItem;

ARRAY_DEF(MenuItemArray, MenuItem, M_POD_OPLIST); //-V658

#define M_OPL_MenuItemArray_t() ARRAY_OPLIST(MenuItemArray, M_POD_OPLIST)

typedef struct {
    MenuItemArray_t items;
    size_t position;
} MenuModel;

static void menu_process_up(Menu* menu);
static void menu_process_down(Menu* menu);
static void menu_process_ok(Menu* menu);

// Nikita's main menu is horizontal: a row of icons with the selected one
// centered and lifted, the name above it, and a dotted horizontal scrollbar
// below. The idea is borrowed from Momentum's horizontal menus, but drawn as
// Nikita's own -- a clean 1-bit layout with none of the console chrome, and
// nothing pulled in beyond this file (no settings subsystem, no core GUI
// additions), so it cannot disturb anything else in the firmware.

// Draw an item's icon centered inside a w*h cell at (x, y).
static void menu_centered_icon(
    Canvas* canvas,
    MenuItem* item,
    int32_t x,
    int32_t y,
    size_t width,
    size_t height) {
    int32_t icon_w = icon_animation_get_width(item->icon);
    int32_t icon_h = icon_animation_get_height(item->icon);
    canvas_draw_icon_animation(
        canvas,
        x + ((int32_t)width - icon_w) / 2,
        y + ((int32_t)height - icon_h) / 2,
        item->icon);
}

// A dotted horizontal scrollbar with a position block, mirroring the vertical
// one's look. Kept local so elements.c (and the SDK it exports) stay untouched.
static void menu_scrollbar_horizontal(
    Canvas* canvas,
    int32_t x,
    int32_t y,
    size_t width,
    size_t pos,
    size_t total) {
    for(size_t i = x; i < width + x; i += 2) {
        canvas_draw_dot(canvas, i, y);
    }
    if(total) {
        float block_w = ((float)width) / total;
        canvas_draw_box(
            canvas, x + (int32_t)(block_w * pos), y - 1, MAX((int32_t)block_w, 1), 3);
    }
}

static void menu_draw_callback(Canvas* canvas, void* _model) {
    MenuModel* model = _model;

    canvas_clear(canvas);

    size_t position = model->position;
    size_t items_count = MenuItemArray_size(model->items);
    if(items_count) {
        MenuItem* item;
        size_t shift_position;

        const int32_t center_x = 64;
        const int32_t row_y = 42;
        const int32_t pitch = 30;

        // The row of icons first, so the title bar and its pointer sit cleanly
        // on top. Five slots: two either side of the centered selection, the
        // modulo wrapping so the row stays continuous at the ends.
        for(int8_t i = -2; i <= 2; i++) {
            shift_position = (position + items_count + i) % items_count;
            item = MenuItemArray_get(model->items, shift_position);

            if(i == 0) {
                // Selected: a bold, larger cell.
                const size_t w = 32, h = 32;
                int32_t cx = center_x;
                int32_t cy = row_y;
                elements_bold_rounded_frame(canvas, cx - w / 2, cy - h / 2, w, h);
                menu_centered_icon(canvas, item, cx - w / 2, cy - h / 2, w, h);
            } else {
                const size_t w = 24, h = 26;
                int32_t cx = center_x + pitch * i;
                int32_t cy = row_y;
                elements_slightly_rounded_frame(canvas, cx - w / 2, cy - h / 2, w, h);
                menu_centered_icon(canvas, item, cx - w / 2, cy - h / 2, w, h);
            }
        }

        // Title bar across the top, carrying the selected item's name, with a
        // small pointer notched into its underside aimed at the selected icon.
        // This is the piece that lifts the menu out of "a plain row of boxes"
        // -- the Momentum silhouette, drawn clean and without the console text.
        item = MenuItemArray_get(model->items, position);
        elements_bold_rounded_frame(canvas, 0, 0, 128, 17);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignCenter, item->label);

        // Pointer: two black edges down to a tip, the bar's bottom line erased
        // between them so it reads as one shape opening toward the icon.
        canvas_draw_line(canvas, 60, 16, 64, 23);
        canvas_draw_line(canvas, 64, 23, 68, 16);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_line(canvas, 61, 16, 67, 16);
        canvas_set_color(canvas, ColorBlack);

        menu_scrollbar_horizontal(canvas, 2, 62, 124, position, items_count);
    } else {
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Empty");
    }
}

static bool menu_input_callback(InputEvent* event, void* context) {
    Menu* menu = context;
    bool consumed = false;

    // The menu is horizontal, so Left/Right move through it. Up/Down are kept
    // as aliases so nothing that still sends them (and muscle memory) breaks.
    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        if(event->key == InputKeyLeft || event->key == InputKeyUp) {
            consumed = true;
            menu_process_up(menu);
        } else if(event->key == InputKeyRight || event->key == InputKeyDown) {
            consumed = true;
            menu_process_down(menu);
        } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
            consumed = true;
            menu_process_ok(menu);
        }
    }

    return consumed;
}

static void menu_enter(void* context) {
    Menu* menu = context;
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(MenuItemArray_size(model->items)) {
                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_start(item->icon);
            }
        },
        false);
}

static void menu_exit(void* context) {
    Menu* menu = context;
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(MenuItemArray_size(model->items)) {
                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_stop(item->icon);
            }
        },
        false);
}

Menu* menu_alloc(void) {
    Menu* menu = malloc(sizeof(Menu));
    menu->view = view_alloc();
    view_set_context(menu->view, menu);
    view_allocate_model(menu->view, ViewModelTypeLocking, sizeof(MenuModel));
    view_set_draw_callback(menu->view, menu_draw_callback);
    view_set_input_callback(menu->view, menu_input_callback);
    view_set_enter_callback(menu->view, menu_enter);
    view_set_exit_callback(menu->view, menu_exit);

    with_view_model(
        menu->view,
        MenuModel * model,
        {
            MenuItemArray_init(model->items);
            model->position = 0;
        },
        true);

    return menu;
}

void menu_free(Menu* menu) {
    furi_check(menu);

    menu_reset(menu);
    with_view_model(menu->view, MenuModel * model, { MenuItemArray_clear(model->items); }, false);
    view_free(menu->view);

    free(menu);
}

View* menu_get_view(Menu* menu) {
    furi_check(menu);
    return menu->view;
}

void menu_add_item(
    Menu* menu,
    const char* label,
    const Icon* icon,
    uint32_t index,
    MenuItemCallback callback,
    void* context) {
    furi_check(menu);
    furi_check(label);

    MenuItem* item = NULL;
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            item = MenuItemArray_push_new(model->items);
            item->label = label;
            item->icon = icon ? icon_animation_alloc(icon) : icon_animation_alloc(&A_Plugins_14);
            view_tie_icon_animation(menu->view, item->icon);
            item->index = index;
            item->callback = callback;
            item->callback_context = context;
        },
        true);
}

void menu_reset(Menu* menu) {
    furi_check(menu);
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            for
                M_EACH(item, model->items, MenuItemArray_t) {
                    icon_animation_stop(item->icon);
                    icon_animation_free(item->icon);
                }

            MenuItemArray_reset(model->items);
            model->position = 0;
        },
        true);
}

void menu_set_selected_item(Menu* menu, uint32_t index) {
    furi_check(menu);

    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(index < MenuItemArray_size(model->items)) {
                model->position = index;
            }
        },
        true);
}

static void menu_process_up(Menu* menu) {
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(MenuItemArray_size(model->items)) {
                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_stop(item->icon);

                if(model->position > 0) {
                    model->position--;
                } else {
                    model->position = MenuItemArray_size(model->items) - 1;
                }

                item = MenuItemArray_get(model->items, model->position);
                icon_animation_start(item->icon);
            }
        },
        true);
}

static void menu_process_down(Menu* menu) {
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(MenuItemArray_size(model->items)) {
                MenuItem* item = MenuItemArray_get(model->items, model->position);
                icon_animation_stop(item->icon);

                if(model->position < MenuItemArray_size(model->items) - 1) {
                    model->position++;
                } else {
                    model->position = 0;
                }

                item = MenuItemArray_get(model->items, model->position);
                icon_animation_start(item->icon);
            }
        },
        true);
}

static void menu_process_ok(Menu* menu) {
    MenuItem* item = NULL;
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(MenuItemArray_size(model->items)) {
                item = MenuItemArray_get(model->items, model->position);
            }
        },
        true);
    if(item && item->callback) {
        item->callback(item->callback_context, item->index);
    }
}
