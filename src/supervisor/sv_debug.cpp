/*
 * ============================================================
 *   CoCo_ESP32 Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : sv_debug.cpp
 *  Module : Supervisor debug dump — memory dump to serial in S-Record or Intel HEX
 * ============================================================
 */

/*
 * sv_debug.cpp - Debug memory dump sub-screen
 *
 * Allows dumping CoCo memory to the serial port in Motorola S-Record
 * or Intel HEX format.  Uses machine_read() so the dump reflects the
 * current SAM/ROM mapping (exactly what the 6809 sees).
 *
 * UI: arrow keys to navigate fields, left/right to change values,
 * ENTER on "Dump" to send, ESC to go back.
 */

#include "sv_debug.h"
#include "supervisor.h"
#include "sv_render.h"
#include "../core/machine.h"
#include "../utils/debug.h"

// HID usage codes
#define HID_UP    0x52
#define HID_DOWN  0x51
#define HID_LEFT  0x50
#define HID_RIGHT 0x4F
#define HID_ENTER 0x28
#define HID_ESC   0x29

// Bytes per record line
#define DUMP_BYTES_PER_LINE  16

static SV_DebugState dbg;

// ============================================================
// S-Record output (Motorola)
// ============================================================

static void srec_send_header(uint16_t start, uint16_t end) {
    // S0 header record with module name
    const char* name = "COCODUMP";
    uint8_t len = strlen(name) + 3;  // count + addr(2) + data + checksum
    uint8_t sum = len;
    sum += 0; sum += 0;  // address 0000
    Serial.printf("S0%02X0000", len);
    for (int i = 0; name[i]; i++) {
        Serial.printf("%02X", (uint8_t)name[i]);
        sum += (uint8_t)name[i];
    }
    Serial.printf("%02X\r\n", (uint8_t)~sum);
}

static void srec_send_data(uint16_t addr, uint16_t end) {
    while (addr <= end) {
        uint16_t remain = end - addr + 1;
        uint8_t count = (remain > DUMP_BYTES_PER_LINE) ? DUMP_BYTES_PER_LINE : remain;

        uint8_t byte_count = count + 3;  // data + 2 addr + 1 checksum
        uint8_t sum = byte_count;
        sum += (addr >> 8) & 0xFF;
        sum += addr & 0xFF;

        Serial.printf("S1%02X%04X", byte_count, addr);
        for (uint8_t i = 0; i < count; i++) {
            uint8_t b = machine_read(addr + i);
            Serial.printf("%02X", b);
            sum += b;
        }
        Serial.printf("%02X\r\n", (uint8_t)~sum);

        addr += count;
        if (addr == 0) break;  // wrapped past 0xFFFF
    }
}

static void srec_send_eof(uint16_t start) {
    // S9 end record with execution start address
    uint8_t sum = 3;  // byte count
    sum += (start >> 8) & 0xFF;
    sum += start & 0xFF;
    Serial.printf("S903%04X%02X\r\n", start, (uint8_t)~sum);
}

// ============================================================
// Intel HEX output
// ============================================================

static void ihex_send_data(uint16_t addr, uint16_t end) {
    while (addr <= end) {
        uint16_t remain = end - addr + 1;
        uint8_t count = (remain > DUMP_BYTES_PER_LINE) ? DUMP_BYTES_PER_LINE : remain;

        uint8_t sum = count;
        sum += (addr >> 8) & 0xFF;
        sum += addr & 0xFF;
        sum += 0x00;  // record type: data

        Serial.printf(":%02X%04X00", count, addr);
        for (uint8_t i = 0; i < count; i++) {
            uint8_t b = machine_read(addr + i);
            Serial.printf("%02X", b);
            sum += b;
        }
        Serial.printf("%02X\r\n", (uint8_t)(-(int8_t)sum));

        addr += count;
        if (addr == 0) break;  // wrapped past 0xFFFF
    }
}

static void ihex_send_eof(void) {
    Serial.print(":00000001FF\r\n");
}

// ============================================================
// Dump execution
// ============================================================

static void execute_dump(Supervisor_t* sv) {
    if (!sv->machine) return;

    uint16_t start = dbg.start_addr;
    uint16_t end   = dbg.end_addr;

    if (end < start) {
        // Swap if reversed
        uint16_t tmp = start;
        start = end;
        end = tmp;
    }

    uint32_t size = (uint32_t)(end - start) + 1;
    const char* fmt_name = (dbg.format == SV_DUMP_SREC) ? "Motorola S-Record" : "Intel HEX";

    // Print header banner
    Serial.println();
    Serial.println("========================================");
    Serial.printf( "  CoCo ESP32 MEMORY DUMP\r\n");
    Serial.printf( "  Format : %s\r\n", fmt_name);
    Serial.printf( "  Range  : $%04X - $%04X (%lu bytes)\r\n", start, end, (unsigned long)size);
    Serial.println("========================================");

    if (dbg.format == SV_DUMP_SREC) {
        srec_send_header(start, end);
        srec_send_data(start, end);
        srec_send_eof(start);
    } else {
        ihex_send_data(start, end);
        ihex_send_eof();
    }

    Serial.println("========== END OF DUMP =================");
    Serial.println();

    DEBUG_PRINTF("Debug dump: %s $%04X-$%04X (%lu bytes)", fmt_name, start, end, (unsigned long)size);
}

// ============================================================
// Public API
// ============================================================

void sv_debug_init(Supervisor_t* sv) {
    dbg.active_field = SV_DBG_FORMAT;
    dbg.format       = SV_DUMP_SREC;
    dbg.start_addr   = 0x0000;
    dbg.end_addr     = 0x00FF;
    dbg.hex_digit    = 0;
    dbg.dumping      = false;
}

void sv_debug_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_UP:
            if (dbg.active_field > 0) {
                dbg.active_field = (SV_DebugField)(dbg.active_field - 1);
                dbg.hex_digit = 0;
                sv->needs_redraw = true;
            }
            break;

        case HID_DOWN:
            if (dbg.active_field < SV_DBG_FIELD_COUNT - 1) {
                dbg.active_field = (SV_DebugField)(dbg.active_field + 1);
                dbg.hex_digit = 0;
                sv->needs_redraw = true;
            }
            break;

        case HID_LEFT:
            switch (dbg.active_field) {
                case SV_DBG_FORMAT:
                    dbg.format = (dbg.format == SV_DUMP_SREC) ? SV_DUMP_IHEX : SV_DUMP_SREC;
                    sv->needs_redraw = true;
                    break;
                case SV_DBG_START_ADDR:
                case SV_DBG_END_ADDR: {
                    // Move cursor left through hex digits
                    if (dbg.hex_digit > 0) dbg.hex_digit--;
                    sv->needs_redraw = true;
                    break;
                }
                default: break;
            }
            break;

        case HID_RIGHT:
            switch (dbg.active_field) {
                case SV_DBG_FORMAT:
                    dbg.format = (dbg.format == SV_DUMP_SREC) ? SV_DUMP_IHEX : SV_DUMP_SREC;
                    sv->needs_redraw = true;
                    break;
                case SV_DBG_START_ADDR:
                case SV_DBG_END_ADDR: {
                    if (dbg.hex_digit < 3) dbg.hex_digit++;
                    sv->needs_redraw = true;
                    break;
                }
                default: break;
            }
            break;

        case HID_ENTER:
            if (dbg.active_field == SV_DBG_EXECUTE) {
                dbg.dumping = true;
                sv->needs_redraw = true;
                execute_dump(sv);
                dbg.dumping = false;
                sv->needs_redraw = true;
            }
            break;

        case HID_ESC:
            sv->state = SV_MAIN_MENU;
            sv->menu_cursor = 0;
            sv->needs_redraw = true;
            break;

        default: {
            // Handle hex digit input (0-9, A-F) for address fields
            if (dbg.active_field != SV_DBG_START_ADDR &&
                dbg.active_field != SV_DBG_END_ADDR) break;

            int nibble_val = -1;

            // HID usage: 0x04=A .. 0x09=F, 0x27=0, 0x1E=1 .. 0x26=9
            if (hid_usage >= 0x04 && hid_usage <= 0x09) {
                nibble_val = 0x0A + (hid_usage - 0x04);  // A=10..F=15
            } else if (hid_usage == 0x27) {
                nibble_val = 0;  // '0'
            } else if (hid_usage >= 0x1E && hid_usage <= 0x26) {
                nibble_val = 1 + (hid_usage - 0x1E);     // 1..9
            }

            if (nibble_val >= 0 && nibble_val <= 0x0F) {
                uint16_t* addr = (dbg.active_field == SV_DBG_START_ADDR)
                                 ? &dbg.start_addr : &dbg.end_addr;
                int shift = (3 - dbg.hex_digit) * 4;
                *addr = (*addr & ~(0xF << shift)) | (nibble_val << shift);

                // Auto-advance to next digit
                if (dbg.hex_digit < 3) dbg.hex_digit++;
                sv->needs_redraw = true;
            }
            break;
        }
    }
}

void sv_debug_render(Supervisor_t* sv) {
    sv_render_frame("Debug Dump", "Up/Dn L/R 0-F  ENT=Dump  ESC=Back");

    const char* fmt_str = (dbg.format == SV_DUMP_SREC) ? "< S-Record >" : "< Intel HEX >";
    char start_str[16], end_str[16];

    // Format address strings with cursor indicator
    if (dbg.active_field == SV_DBG_START_ADDR) {
        // Show which nibble is active with brackets
        snprintf(start_str, sizeof(start_str), "$%04X [%d]", dbg.start_addr, dbg.hex_digit);
    } else {
        snprintf(start_str, sizeof(start_str), "$%04X", dbg.start_addr);
    }

    if (dbg.active_field == SV_DBG_END_ADDR) {
        snprintf(end_str, sizeof(end_str), "$%04X [%d]", dbg.end_addr, dbg.hex_digit);
    } else {
        snprintf(end_str, sizeof(end_str), "$%04X", dbg.end_addr);
    }

    // Row layout
    sv_render_menu_item(0, "Format:",      fmt_str,   dbg.active_field == SV_DBG_FORMAT);
    sv_render_menu_item(2, "Start Addr:",  start_str, dbg.active_field == SV_DBG_START_ADDR);
    sv_render_menu_item(4, "End Addr:",    end_str,   dbg.active_field == SV_DBG_END_ADDR);

    if (dbg.dumping) {
        sv_render_centered_item(6, "Dumping...", SV_COLOR_WARN);
    } else {
        sv_render_menu_item(6, ">> Send Dump to Serial <<", NULL, dbg.active_field == SV_DBG_EXECUTE);
    }

    // Show size preview
    uint16_t s = dbg.start_addr, e = dbg.end_addr;
    if (e < s) { uint16_t t = s; s = e; e = t; }
    uint32_t size = (uint32_t)(e - s) + 1;
    char size_str[32];
    snprintf(size_str, sizeof(size_str), "Size: %lu bytes", (unsigned long)size);
    sv_render_centered_item(8, size_str, SV_COLOR_DIM);
}
