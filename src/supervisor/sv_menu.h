/*
 * ============================================================
 *   CoCo_ESP32 Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : sv_menu.h
 *  Module : Supervisor OSD main menu interface
 * ============================================================
 */

/*
 * sv_menu.h - Main menu for supervisor OSD
 */

#ifndef SV_MENU_H
#define SV_MENU_H

#include <stdint.h>

// Forward declaration
struct Supervisor_t;

enum SV_MenuAction : uint8_t {
    SV_ACT_MOUNT_DISK,
    SV_ACT_DISK_MANAGER,
    SV_ACT_MACHINE_SELECT,
    SV_ACT_RESET,
    SV_ACT_SETTINGS,
    SV_ACT_ABOUT,
    SV_ACT_DEBUG_DUMP,
    SV_ACT_RESUME,
};

struct SV_MenuItem {
    const char* label;
    SV_MenuAction action;
    const char* value;  // Right-aligned value text, NULL if none
};

void sv_menu_init(Supervisor_t* sv);
void sv_menu_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_menu_render(Supervisor_t* sv);
void sv_menu_update_values(Supervisor_t* sv);

#endif // SV_MENU_H
