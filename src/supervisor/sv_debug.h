/*
 * ============================================================
 *   CoCo_ESP32 Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : sv_debug.h
 *  Module : Supervisor debug dump — memory dump to serial in S-Record or Intel HEX
 * ============================================================
 */

#ifndef SV_DEBUG_H
#define SV_DEBUG_H

#include <stdint.h>

struct Supervisor_t;

// Debug dump sub-screen states
enum SV_DebugField : uint8_t {
    SV_DBG_FORMAT,       // Select: S-Record or Intel HEX
    SV_DBG_START_ADDR,   // Edit 4-digit hex address
    SV_DBG_END_ADDR,     // Edit 4-digit hex address
    SV_DBG_EXECUTE,      // "Dump" button
    SV_DBG_FIELD_COUNT
};

enum SV_DumpFormat : uint8_t {
    SV_DUMP_SREC = 0,
    SV_DUMP_IHEX = 1,
};

struct SV_DebugState {
    SV_DebugField  active_field;
    SV_DumpFormat  format;
    uint16_t       start_addr;
    uint16_t       end_addr;
    uint8_t        hex_digit;      // Which nibble is being edited (0-3)
    bool           dumping;        // True while dump is in progress
};

void sv_debug_init(Supervisor_t* sv);
void sv_debug_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_debug_render(Supervisor_t* sv);

#endif // SV_DEBUG_H
