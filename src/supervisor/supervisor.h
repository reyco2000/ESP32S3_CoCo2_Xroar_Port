/*
 * ============================================================
 *   CoCo_ESP32 Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : supervisor.h
 *  Module : OSD supervisor interface — overlay lifecycle and event dispatch
 * ============================================================
 */

/*
 * supervisor.h - On-Screen Display (OSD) supervisor for CoCo_ESP32
 *
 * F1 toggles the supervisor overlay. While active, emulation is paused.
 * Provides disk mounting, machine reset, and settings via menu UI.
 */

#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include <stdint.h>
#include <stdbool.h>
#include "../core/machine.h"
#include "sv_disk.h"

enum SV_State : uint8_t {
    SV_INACTIVE = 0,
    SV_MAIN_MENU,
    SV_FILE_BROWSER,
    SV_DISK_MANAGER,
    SV_MACHINE_SELECT,
    SV_SETTINGS,
    SV_ABOUT,
    SV_DEBUG_DUMP,
    SV_CONFIRM_DIALOG,
};

struct SV_FileEntry;

typedef struct Supervisor_t {
    SV_State state;
    SV_State prev_state;

    Machine* machine;

    // Framebuffer snapshot (PSRAM)
    uint16_t* emu_snapshot;

    // Menu state
    int8_t   menu_cursor;
    int8_t   menu_scroll_offset;
    uint8_t  menu_item_count;

    // File browser state
    char     current_path[256];
    int16_t  file_cursor;
    int16_t  file_scroll_offset;
    int16_t  file_count;
    uint8_t  target_drive;

    // File entries (allocated once)
    SV_FileEntry* file_entries;

    // Confirm dialog
    const char* confirm_message;
    void (*confirm_callback)(bool accepted, void* ctx);
    void* confirm_context;
    bool  confirm_yes_selected;

    // Rendering
    bool     needs_redraw;
    uint32_t last_blink_ms;
    bool     blink_on;

} Supervisor_t;

// Public API
void supervisor_init(Machine* m);
void supervisor_toggle(void);
bool supervisor_is_active(void);

void supervisor_on_key(uint8_t hid_usage, bool pressed);

// Returns true if supervisor consumed the frame (skip emulation)
bool supervisor_update_and_render(void);

void supervisor_quick_mount_last_disk(Machine* m);

void supervisor_save_state(void);
void supervisor_load_state(void);

// Access global supervisor (for disk manager sub-screens)
Supervisor_t* supervisor_get(void);

#endif // SUPERVISOR_H
