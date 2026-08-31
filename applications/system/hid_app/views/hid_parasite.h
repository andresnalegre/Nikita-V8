#pragma once

#include <gui/view.h>

typedef struct Hid Hid;
typedef struct HidParasite HidParasite;

HidParasite* hid_parasite_alloc(Hid* hid);

void hid_parasite_free(HidParasite* hid_parasite);

View* hid_parasite_get_view(HidParasite* hid_parasite);

void hid_parasite_set_connected_status(HidParasite* hid_parasite, bool connected);
