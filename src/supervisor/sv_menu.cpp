/*
 * ============================================================
 *   CoCo_ESP32 Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : sv_menu.cpp
 *  Module : Supervisor OSD main menu — disk/drive options, reset, and settings
 * ============================================================
 */

/*
 * sv_menu.cpp - Main menu for supervisor OSD
 */

#include "sv_menu.h"
#include "supervisor.h"
#include "sv_filebrowser.h"
#include "sv_render.h"

// HID usage codes
#define HID_UP    0x52
#define HID_DOWN  0x51
#define HID_ENTER 0x28
#define HID_ESC   0x29
#define HID_F1    0x3A

static SV_MenuItem menu_items[] = {
    { "Browse/Mount Disk", SV_ACT_MOUNT_DISK,    NULL },
    { "Select Drive - Eject Disk",       SV_ACT_DISK_MANAGER,   NULL },
    { "Machine:",           SV_ACT_MACHINE_SELECT, "CoCo 2" },
    { "Reset Machine",      SV_ACT_RESET,          NULL },
    { "Debug Dump",         SV_ACT_DEBUG_DUMP,     NULL },
    { "About",              SV_ACT_ABOUT,          NULL },
};

static const int MENU_COUNT = sizeof(menu_items) / sizeof(menu_items[0]);

void sv_menu_init(Supervisor_t* sv) {
    sv->menu_cursor = 0;
    sv->menu_scroll_offset = 0;
    sv->menu_item_count = MENU_COUNT;
}

void sv_menu_update_values(Supervisor_t* sv) {
    // Update machine name based on config
    menu_items[2].value = "CoCo 2";
}

static void execute_action(Supervisor_t* sv, SV_MenuAction action) {
    switch (action) {
        case SV_ACT_MOUNT_DISK:
            sv->target_drive = 0;
            sv->prev_state = sv->state;
            sv->state = SV_FILE_BROWSER;
            sv_filebrowser_open(sv, sv->current_path, sv->target_drive);
            break;

        case SV_ACT_DISK_MANAGER:
            sv->prev_state = sv->state;
            sv->state = SV_DISK_MANAGER;
            sv->menu_cursor = 0;
            sv->needs_redraw = true;
            break;

        case SV_ACT_MACHINE_SELECT:
            // TODO: machine selection sub-menu
            break;

        case SV_ACT_RESET:
            sv->prev_state = sv->state;
            sv->state = SV_CONFIRM_DIALOG;
            sv->confirm_message = "Reset machine?";
            sv->confirm_yes_selected = false;
            sv->confirm_callback = [](bool accepted, void* ctx) {
                Supervisor_t* s = (Supervisor_t*)ctx;
                if (accepted && s->machine) {
                    machine_reset(s->machine);
                }
                s->state = SV_INACTIVE;
                s->needs_redraw = true;
            };
            sv->confirm_context = sv;
            sv->needs_redraw = true;
            break;

        case SV_ACT_DEBUG_DUMP:
            sv->prev_state = sv->state;
            sv->state = SV_DEBUG_DUMP;
            sv->needs_redraw = true;
            break;

        case SV_ACT_ABOUT:
            sv->prev_state = sv->state;
            sv->state = SV_ABOUT;
            sv->needs_redraw = true;
            break;

        case SV_ACT_RESUME:
            supervisor_toggle();
            break;

        default:
            break;
    }
}

void sv_menu_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_UP:
            if (sv->menu_cursor > 0) {
                sv->menu_cursor--;
                sv->needs_redraw = true;
            }
            break;

        case HID_DOWN:
            if (sv->menu_cursor < sv->menu_item_count - 1) {
                sv->menu_cursor++;
                sv->needs_redraw = true;
            }
            break;

        case HID_ENTER:
            execute_action(sv, menu_items[sv->menu_cursor].action);
            break;

        case HID_ESC:
        case HID_F1:
            supervisor_toggle();
            break;
    }
}

void sv_menu_render(Supervisor_t* sv) {
    sv_render_frame("*CoCo ESP32 SUPERVISOR*", "Up/Dn -  ENTER - F1/ESC");

    // Vertically center: content area fits ~8 rows, offset to center items
    int content_rows = (SV_BORDER_H - SV_TITLE_H - SV_FOOTER_H - 8) / SV_ITEM_H;
    int offset = (content_rows - sv->menu_item_count) / 2;
    if (offset < 0) offset = 0;

    for (int i = 0; i < sv->menu_item_count; i++) {
        sv_render_menu_item(i + offset, menu_items[i].label, menu_items[i].value,
                           i == sv->menu_cursor);
    }
}
