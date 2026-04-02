/*
 * ============================================================
 *   CoCo_ESP32 Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : supervisor.cpp
 *  Module : OSD supervisor — F1-activated overlay with menu, disk mounting, and machine reset
 * ============================================================
 */

/*
 * supervisor.cpp - On-Screen Display supervisor for CoCo_ESP32
 *
 * Lifecycle: F1 toggles active. While active, emulation paused.
 * Framebuffer snapshot saved on activate, restored on deactivate.
 */

#include "supervisor.h"
#include "sv_menu.h"
#include "sv_filebrowser.h"
#include "sv_disk.h"
#include "sv_debug.h"
#include "sv_render.h"
#include "../hal/hal.h"
#include "../utils/debug.h"
#include <Preferences.h>

// External TFT instance from hal_video.cpp
extern TFT_eSPI* hal_video_get_tft(void);

// Global supervisor instance
static Supervisor_t sv;

// HID usage codes for disk manager
#define HID_UP    0x52
#define HID_DOWN  0x51
#define HID_ENTER 0x28
#define HID_ESC   0x29
#define HID_M     0x10
#define HID_E     0x08
#define HID_F     0x09
#define HID_LEFT  0x50
#define HID_RIGHT 0x4F

// ============================================================
// Framebuffer snapshot — DISABLED
// ============================================================
// readRect() requires MISO wired (PIN_TFT_MISO = -1 on this board)
// and 153KB PSRAM (not available on this N16R8 module).
// Instead, on deactivate we simply let the next emulated frame
// repaint the display naturally.

static void capture_snapshot(void) {
    // No-op: cannot read back from TFT without MISO
}

static void restore_snapshot(void) {
    // Clear the OSD area so no ghost pixels remain in border margins.
    // The emulator sprite covers (32,24)-(288,216) and will repaint
    // that region on the next frame. Clear the full OSD rect to be safe.
    TFT_eSPI* tft = hal_video_get_tft();
    if (tft) {
        tft->fillRect(SV_BORDER_X, SV_BORDER_Y, SV_BORDER_W, SV_BORDER_H, TFT_BLACK);
    }
}

// ============================================================
// Disk manager sub-screen
// ============================================================

static void disk_manager_on_key(Supervisor_t* s, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_UP:
            if (s->menu_cursor > 0) {
                s->menu_cursor--;
                s->needs_redraw = true;
            }
            break;

        case HID_DOWN:
            if (s->menu_cursor < SV_DISK_MAX_DRIVES - 1) {
                s->menu_cursor++;
                s->needs_redraw = true;
            }
            break;

        case HID_ENTER:
            // Context-sensitive: eject if mounted, mount if empty
            if (s->machine && sv_disk_is_mounted(&s->machine->fdc, s->menu_cursor)) {
                sv_disk_eject(&s->machine->fdc, s->menu_cursor);
                supervisor_save_state();
                s->needs_redraw = true;
            } else {
                s->target_drive = s->menu_cursor;
                s->prev_state = SV_DISK_MANAGER;
                s->state = SV_FILE_BROWSER;
                sv_filebrowser_open(s, s->current_path, s->target_drive);
                s->needs_redraw = true;
            }
            break;

        case HID_M:
            // Mount: always open file browser (replaces if already mounted)
            s->target_drive = s->menu_cursor;
            s->prev_state = SV_DISK_MANAGER;
            s->state = SV_FILE_BROWSER;
            sv_filebrowser_open(s, s->current_path, s->target_drive);
            s->needs_redraw = true;
            break;

        case HID_E:
            // Eject selected drive
            if (s->machine && sv_disk_is_mounted(&s->machine->fdc, s->menu_cursor)) {
                sv_disk_eject(&s->machine->fdc, s->menu_cursor);
                supervisor_save_state();
                s->needs_redraw = true;
            }
            break;

        case HID_F:
            // Flush selected drive to SD (save without ejecting)
            if (s->machine && sv_disk_is_mounted(&s->machine->fdc, s->menu_cursor)) {
                sv_disk_flush(&s->machine->fdc, s->menu_cursor);
                s->needs_redraw = true;
            }
            break;

        case HID_ESC:
            s->state = SV_MAIN_MENU;
            s->menu_cursor = 1;  // Disk Manager menu item
            s->needs_redraw = true;
            break;
    }
}

static void disk_manager_render(Supervisor_t* s) {
    sv_render_frame("Disk Manager", "ENT=Mount/Eject M=Mount E=Eject F=Flush");

    for (int i = 0; i < SV_DISK_MAX_DRIVES; i++) {
        char label[48];
        const char* value = NULL;
        bool mounted = s->machine && sv_disk_is_mounted(&s->machine->fdc, i);

        if (mounted) {
            const char* path = sv_disk_get_path(&s->machine->fdc, i);
            const char* fname = strrchr(path, '/');
            fname = fname ? fname + 1 : path;
            snprintf(label, sizeof(label), "Drive %d: %s", i, fname);

            static char val_buf[32];
            SV_DiskImage* img = &s->machine->fdc.drives[i];
            snprintf(val_buf, sizeof(val_buf), "%dT/%dS", img->tracks, img->sectors_per_track);
            value = val_buf;
        } else {
            snprintf(label, sizeof(label), "Drive %d: (empty)", i);
        }

        sv_render_menu_item(i * 2, label, value, i == s->menu_cursor);

        // Second line with geometry (if mounted)
        if (mounted) {
            SV_DiskImage* img = &s->machine->fdc.drives[i];
            char detail[48];
            snprintf(detail, sizeof(detail), "  %luK %s",
                     (unsigned long)(img->image_size / 1024),
                     img->dirty ? "[modified]" : "");
            sv_render_menu_item(i * 2 + 1, detail, NULL, false);
        }
    }
}

// ============================================================
// About screen
// ============================================================

// COCOBYTE LOGO — 91x12 pixels, horizontal bitmap (MSB first, 12 bytes/row)
static const unsigned char bitmap_cocobyte[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf0, 0x01, 0x1b, 0xf0, 0x01, 0x1b, 0xfe, 0xf9, 0x7c, 0xff, 0xfd, 0x03,
    0xf8, 0x83, 0x3b, 0xf8, 0x83, 0x3b, 0xfe, 0xfb, 0x7c, 0xff, 0xfd, 0x03,
    0xfc, 0xc7, 0x7b, 0xfc, 0xc7, 0x7b, 0xfe, 0xfb, 0x7c, 0xff, 0xfd, 0x03,
    0xfe, 0xef, 0xfb, 0xfe, 0xef, 0xfb, 0xf0, 0xfb, 0x7f, 0xff, 0x7d, 0x00,
    0xfe, 0xe1, 0xfb, 0xfe, 0xe1, 0xfb, 0xfe, 0xfb, 0x7f, 0x7c, 0xfc, 0x03,
    0x7e, 0xe0, 0xfb, 0x7e, 0xe0, 0xfb, 0xfe, 0xf1, 0x3f, 0x7c, 0xfc, 0x03,
    0xfe, 0xe1, 0xfb, 0xfe, 0xe1, 0xfb, 0xfe, 0xe3, 0x1f, 0x7c, 0xfc, 0x03,
    0xfe, 0xef, 0xfb, 0xfe, 0xef, 0xfb, 0xe0, 0xc3, 0x0f, 0x7c, 0x7c, 0x00,
    0xfc, 0xc7, 0x7b, 0xfc, 0xc7, 0x7b, 0xfe, 0xc3, 0x0f, 0x7c, 0xfc, 0x07,
    0xf8, 0x83, 0x3b, 0xf8, 0x83, 0x3b, 0xfe, 0xc3, 0x0f, 0x7c, 0xfc, 0x07,
    0xf0, 0x01, 0x1b, 0xf0, 0x01, 0x1b, 0xfe, 0xc1, 0x0f, 0x7c, 0xfc, 0x07
};
#define LOGO_W  91
#define LOGO_H  12
#define LOGO_ROW_BYTES  12  // ceil(91/8)

// Draw monochrome XBM bitmap scaled to TFT (set bits = pixel on)
static void draw_mono_bitmap_scaled(TFT_eSPI* tft, int x0, int y0,
                                    const unsigned char* bmp, int w, int h,
                                    int scale, uint16_t color) {
    tft->startWrite();
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint8_t b = pgm_read_byte(&bmp[row * LOGO_ROW_BYTES + (col >> 3)]);
            if (b & (1 << (col & 7))) {  // XBM format: LSB first
                if (scale == 1) {
                    tft->drawPixel(x0 + col, y0 + row, color);
                } else {
                    tft->fillRect(x0 + col * scale, y0 + row * scale,
                                  scale, scale, color);
                }
            }
        }
    }
    tft->endWrite();
}

// About screen color palette
#define ABOUT_GREEN      0x07E0  // Pure green
#define ABOUT_DK_GREEN   0x03A0  // Dark green (decorative lines)
#define ABOUT_CYAN       0x07FF  // Cyan accent
#define ABOUT_AMBER      0xFBE0  // Warm amber for version

static void about_render(Supervisor_t* s) {
    TFT_eSPI* tft = hal_video_get_tft();
    if (!tft) return;

    // Use the standard frame but with custom title
    sv_render_frame("About", "ESC = Back");

    tft->startWrite();

    // --- Layout constants (pixel positions within the OSD frame) ---
    const int cx  = SV_BORDER_X + SV_BORDER_W / 2;  // horizontal center
    const int x1  = SV_BORDER_X + 6;                 // left margin
    const int x2  = SV_BORDER_X + SV_BORDER_W - 6;  // right margin
    const int top = SV_CONTENT_Y + 2;                // top of content area

    // --- Row 1: Logo (scaled 2x = 182x24) centered with green tint bg ---
    const int logo_scale = 2;
    const int logo_sw = LOGO_W * logo_scale;  // 182
    const int logo_sh = LOGO_H * logo_scale;  // 24
    const int logo_x = cx - logo_sw / 2;
    const int logo_y = top;

    // Subtle dark background behind logo
    tft->fillRect(logo_x - 4, logo_y - 2, logo_sw + 8, logo_sh + 4, 0x0120);
    tft->drawRect(logo_x - 4, logo_y - 2, logo_sw + 8, logo_sh + 4, ABOUT_DK_GREEN);

    tft->endWrite();

    // Draw logo (has its own startWrite/endWrite)
    draw_mono_bitmap_scaled(tft, logo_x, logo_y, bitmap_cocobyte,
                            LOGO_W, LOGO_H, logo_scale, ABOUT_GREEN);

    tft->startWrite();

    // --- Row 2: Project subtitle ---
    int y = logo_y + logo_sh + 8;
    tft->setTextFont(1);  // 8px font for subtitle
    tft->setTextColor(ABOUT_CYAN, SV_COLOR_BG);
    tft->setTextDatum(TC_DATUM);
    tft->drawString("CoCo 2 Emulator for the ESP32-S3", cx, y);

    // --- Decorative separator ---
    y += 14;
    tft->drawFastHLine(x1 + 20, y, (x2 - x1) - 40, ABOUT_DK_GREEN);
    tft->drawPixel(x1 + 18, y, ABOUT_GREEN);
    tft->drawPixel(x2 - 18, y, ABOUT_GREEN);

    // --- Row 3: Copyright ---
    y += 8;
    tft->setTextFont(1);
    tft->setTextColor(SV_COLOR_TEXT, SV_COLOR_BG);
    tft->setTextDatum(TC_DATUM);
    tft->drawString("(C) 2026 Reinaldo Torres", cx, y);

    // --- Row 4: XRoar credit ---
    y += 12;
    tft->setTextColor(SV_COLOR_DIM, SV_COLOR_BG);
    tft->drawString("Based on XRoar by Ciaran Anscomb", cx, y);

    // --- Row 5: Claude credit ---
    y += 12;
    tft->setTextColor(SV_COLOR_DIM, SV_COLOR_BG);
    tft->drawString("Co-developed with Claude Code", cx, y);

    // --- Decorative separator ---
    y += 12;
    tft->drawFastHLine(x1 + 20, y, (x2 - x1) - 40, ABOUT_DK_GREEN);
    tft->drawPixel(x1 + 18, y, ABOUT_GREEN);
    tft->drawPixel(x2 - 18, y, ABOUT_GREEN);

    // --- Row 6: Version badge ---
    y += 6;
    const char* ver_str = "Beta 1.0  Build 25.03.2026";
    int ver_w = tft->textWidth(ver_str) + 16;
    int ver_x = cx - ver_w / 2;
    tft->fillRect(ver_x, y, ver_w, 13, 0x0120);
    tft->drawRect(ver_x, y, ver_w, 13, ABOUT_DK_GREEN);
    tft->setTextColor(ABOUT_AMBER, 0x0120);
    tft->drawString(ver_str, cx, y + 3);

    // --- Row 7: System stats ---
    y += 20;
    char stats[64];
    snprintf(stats, sizeof(stats), "Heap:%dK  PSRAM:%dK",
             ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
    tft->setTextColor(ABOUT_DK_GREEN, SV_COLOR_BG);
    tft->drawString(stats, cx, y);

    tft->setTextDatum(TL_DATUM);
    tft->endWrite();
}

// ============================================================
// Confirm dialog
// ============================================================

static void confirm_on_key(Supervisor_t* s, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_LEFT:
        case HID_RIGHT:
            s->confirm_yes_selected = !s->confirm_yes_selected;
            s->needs_redraw = true;
            break;

        case HID_ENTER:
            if (s->confirm_callback) {
                s->confirm_callback(s->confirm_yes_selected, s->confirm_context);
            }
            break;

        case HID_ESC:
            s->state = s->prev_state;
            s->needs_redraw = true;
            break;
    }
}

// ============================================================
// Public API
// ============================================================

void supervisor_init(Machine* m) {
    memset(&sv, 0, sizeof(Supervisor_t));
    sv.state = SV_INACTIVE;
    sv.machine = m;
    sv.emu_snapshot = nullptr;
    sv.file_entries = nullptr;
    strcpy(sv.current_path, "/");

    // Initialize FDC
    sv_disk_init(&m->fdc);

    // Wire NMI callback
    m->fdc.nmi_callback = [](void* ctx, bool active) {
        Machine* mach = (Machine*)ctx;
        mc6809_nmi(&mach->cpu, active);
    };
    // Wire HALT callback (DRQ synchronization — matches XRoar signal_halt)
    m->fdc.halt_callback = [](void* ctx, bool halted) {
        Machine* mach = (Machine*)ctx;
        mach->cpu.halted = halted;
    };
    m->fdc.callback_context = m;

    // Initialize render engine
    TFT_eSPI* tft = hal_video_get_tft();
    if (tft) {
        sv_render_init(tft);
    }

    sv_menu_init(&sv);
    sv_filebrowser_init(&sv);
    sv_debug_init(&sv);

    DEBUG_PRINT("Supervisor: initialized");
}

void supervisor_toggle(void) {
    if (sv.state == SV_INACTIVE) {
        // Activate
        capture_snapshot();
        sv.state = SV_MAIN_MENU;
        sv.menu_cursor = 0;
        sv.needs_redraw = true;
        sv_menu_update_values(&sv);
        DEBUG_PRINT("Supervisor: activated");
    } else {
        // Deactivate
        restore_snapshot();
        sv.state = SV_INACTIVE;
        hal_video_force_repaint();
        DEBUG_PRINT("Supervisor: deactivated");
    }
}

bool supervisor_is_active(void) {
    return sv.state != SV_INACTIVE;
}

void supervisor_on_key(uint8_t hid_usage, bool pressed) {
    // F1 within supervisor = close
    if (hid_usage == 0x3A && pressed) {
        supervisor_toggle();
        return;
    }

    switch (sv.state) {
        case SV_MAIN_MENU:
            sv_menu_on_key(&sv, hid_usage, pressed);
            break;

        case SV_FILE_BROWSER:
            sv_filebrowser_on_key(&sv, hid_usage, pressed);
            break;

        case SV_DISK_MANAGER:
            disk_manager_on_key(&sv, hid_usage, pressed);
            break;

        case SV_ABOUT:
            if (pressed && (hid_usage == 0x29 || hid_usage == 0x28)) {  // ESC or ENTER
                sv.state = SV_MAIN_MENU;
                sv.needs_redraw = true;
            }
            break;

        case SV_DEBUG_DUMP:
            sv_debug_on_key(&sv, hid_usage, pressed);
            break;

        case SV_CONFIRM_DIALOG:
            confirm_on_key(&sv, hid_usage, pressed);
            break;

        default:
            break;
    }
}

bool supervisor_update_and_render(void) {
    if (sv.state == SV_INACTIVE) return false;

    if (!sv.needs_redraw) {
        delay(16);  // ~60 fps cap while in supervisor
        return true;
    }

    sv.needs_redraw = false;

    switch (sv.state) {
        case SV_MAIN_MENU:
            sv_menu_render(&sv);
            break;

        case SV_FILE_BROWSER:
            sv_filebrowser_render(&sv);
            break;

        case SV_DISK_MANAGER:
            disk_manager_render(&sv);
            break;

        case SV_ABOUT:
            about_render(&sv);
            break;

        case SV_DEBUG_DUMP:
            sv_debug_render(&sv);
            break;

        case SV_CONFIRM_DIALOG:
            sv_render_confirm_dialog(sv.confirm_message, sv.confirm_yes_selected);
            break;

        default:
            break;
    }

    return true;
}

void supervisor_quick_mount_last_disk(Machine* m) {
    Preferences prefs;
    prefs.begin("sv", true);
    String path = prefs.getString("d0_path", "");
    prefs.end();

    if (path.length() > 0) {
        sv_disk_mount(&m->fdc, 0, path.c_str());
        DEBUG_PRINTF("Quick mount: %s", path.c_str());
    }
}

void supervisor_save_state(void) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putString("last_dir", sv.current_path);
    for (int i = 0; i < SV_DISK_MAX_DRIVES; i++) {
        char key[8];
        snprintf(key, sizeof(key), "d%d_path", i);
        if (sv.machine && sv_disk_is_mounted(&sv.machine->fdc, i)) {
            prefs.putString(key, sv_disk_get_path(&sv.machine->fdc, i));
        } else {
            prefs.remove(key);
        }
    }
    prefs.end();
}

void supervisor_load_state(void) {
    Preferences prefs;
    prefs.begin("sv", true);

    String last_dir = prefs.getString("last_dir", "/");
    strncpy(sv.current_path, last_dir.c_str(), sizeof(sv.current_path) - 1);

    bool auto_mount = prefs.getBool("auto_mount", true);
    if (auto_mount && sv.machine) {
        for (int i = 0; i < SV_DISK_MAX_DRIVES; i++) {
            char key[8];
            snprintf(key, sizeof(key), "d%d_path", i);
            String path = prefs.getString(key, "");
            if (path.length() > 0) {
                sv_disk_mount(&sv.machine->fdc, i, path.c_str());
            }
        }
    }

    prefs.end();
}

Supervisor_t* supervisor_get(void) {
    return &sv;
}
