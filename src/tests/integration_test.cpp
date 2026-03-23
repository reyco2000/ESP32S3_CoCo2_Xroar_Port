/*
 * ============================================================
 *   CoCo_ESP32 Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : integration_test.cpp
 *  Module : Integration test framework — LOADM disk verification and VRAM screen inspection
 * ============================================================
 */

/*
 * CoCo ESP32 - Integration Test Framework Implementation
 *
 * LOADM disk verification test: injects LOADM "filename" via keyboard,
 * then compares CoCo RAM with the raw file data read from the disk cache.
 *
 * KEYBOARD MATRIX (verified from BASIC ROM KEYIN routine):
 * hal_keyboard_press(row, col) where row=PA bit, col=PB bit.
 * key_matrix[col] bit row = 0 when pressed (active LOW).
 * BASIC computes key_code = PA_row * 8 + PB_column.
 *
 *         PB0  PB1  PB2  PB3  PB4  PB5  PB6  PB7
 * PA0:     @    A    B    C    D    E    F    G
 * PA1:     H    I    J    K    L    M    N    O
 * PA2:     P    Q    R    S    T    U    V    W
 * PA3:     X    Y    Z   UP  DOWN LEFT RIGHT SPACE
 * PA4:     0    1    2    3    4    5    6    7
 * PA5:     8    9    :    ;    ,    -    .    /
 * PA6:   ENTER CLEAR BREAK ---  ---  ---  ---  SHIFT
 *
 * VRAM text screen: 32x16 chars at $0400, MC6847 internal charset.
 * VRAM byte encoding:
 *   $00-$1F -> characters '@' through '_' (uppercase + symbols)
 *   $20-$3F -> space through '?' (ASCII 0x20-0x3F directly)
 *   $40-$5F -> inverse '@' through '_'
 *   $60-$7F -> inverse space through '?'
 *   $80-$FF -> semigraphics-4 blocks
 */

#include "integration_test.h"
#include "../../config.h"
#include "../hal/hal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <Arduino.h>
#include <SD.h>

// CoCo text screen constants
static const uint16_t TEXT_SCREEN_ADDR = 0x0400;
static const int SCREEN_COLS = 32;
static const int SCREEN_ROWS = 16;
static const int SCREEN_SIZE = SCREEN_COLS * SCREEN_ROWS;

// CoCo SHIFT key: PA6, PB7
static const uint8_t SHIFT_ROW = 6;
static const uint8_t SHIFT_COL = 7;

// ============================================================================
// ASCII -> CoCo matrix position (verified from BASIC ROM KEYIN)
// ============================================================================

bool IntegrationTest::ascii_to_matrix(char c, uint8_t& row, uint8_t& col, bool& shift_out) {
    shift_out = false;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';

    if (c == '@')              { row = 0; col = 0; return true; }
    if (c >= 'A' && c <= 'G')  { row = 0; col = 1 + (c - 'A'); return true; }
    if (c >= 'H' && c <= 'O')  { row = 1; col = c - 'H'; return true; }
    if (c >= 'P' && c <= 'W')  { row = 2; col = c - 'P'; return true; }
    if (c >= 'X' && c <= 'Z')  { row = 3; col = c - 'X'; return true; }
    if (c == ' ')              { row = 3; col = 7; return true; }
    if (c == '0')              { row = 4; col = 0; return true; }
    if (c >= '1' && c <= '7')  { row = 4; col = 1 + (c - '1'); return true; }
    if (c == '8')              { row = 5; col = 0; return true; }
    if (c == '9')              { row = 5; col = 1; return true; }
    if (c == ':')              { row = 5; col = 2; return true; }
    if (c == ';')              { row = 5; col = 3; return true; }
    if (c == ',')              { row = 5; col = 4; return true; }
    if (c == '-')              { row = 5; col = 5; return true; }
    if (c == '.')              { row = 5; col = 6; return true; }
    if (c == '/')              { row = 5; col = 7; return true; }
    if (c == '\n' || c == '\r') { row = 6; col = 0; return true; }

    // Shifted punctuation
    if (c == '!') { row = 4; col = 1; shift_out = true; return true; }
    if (c == '"') { row = 4; col = 2; shift_out = true; return true; }
    if (c == '#') { row = 4; col = 3; shift_out = true; return true; }
    if (c == '$') { row = 4; col = 4; shift_out = true; return true; }
    if (c == '%') { row = 4; col = 5; shift_out = true; return true; }
    if (c == '&') { row = 4; col = 6; shift_out = true; return true; }
    if (c == '\'') { row = 4; col = 7; shift_out = true; return true; }
    if (c == '(') { row = 5; col = 0; shift_out = true; return true; }
    if (c == ')') { row = 5; col = 1; shift_out = true; return true; }
    if (c == '*') { row = 5; col = 2; shift_out = true; return true; }
    if (c == '+') { row = 5; col = 3; shift_out = true; return true; }
    if (c == '<') { row = 5; col = 4; shift_out = true; return true; }
    if (c == '=') { row = 5; col = 5; shift_out = true; return true; }
    if (c == '>') { row = 5; col = 6; shift_out = true; return true; }
    if (c == '?') { row = 5; col = 7; shift_out = true; return true; }

    return false;
}

// ============================================================================
// VRAM byte -> ASCII
// ============================================================================

char IntegrationTest::vram_to_ascii(uint8_t vram_byte) {
    if (vram_byte & 0x80) return '#';
    uint8_t ch = vram_byte & 0x3F;
    if (ch < 0x20) return (char)(ch + 0x40);
    return (char)ch;
}

// ============================================================================
// Constructor
// ============================================================================

IntegrationTest::IntegrationTest(Machine* m) : machine(m) {
    memset(results, 0, sizeof(results));
    memset(key_queue, 0, sizeof(key_queue));
}

// ============================================================================
// Frame execution
// ============================================================================

void IntegrationTest::run_frames(int count) {
    for (int i = 0; i < count; i++) {
        process_key_queue();
        machine_run_frame(machine);
        frame_counter++;
    }
}

// ============================================================================
// Keyboard injection
// ============================================================================

void IntegrationTest::queue_key_event(uint8_t row, uint8_t col, bool shift, bool pressed) {
    if (kq_count >= KEY_QUEUE_SIZE) return;
    key_queue[kq_tail].row = row;
    key_queue[kq_tail].col = col;
    key_queue[kq_tail].shift = shift;
    key_queue[kq_tail].pressed = pressed;
    kq_tail = (kq_tail + 1) % KEY_QUEUE_SIZE;
    kq_count++;
}

void IntegrationTest::release_all_keys() {
    hal_keyboard_release_all();
    key_active = false;
    key_timer = 0;
}

void IntegrationTest::process_key_queue() {
    if (key_timer > 0) { key_timer--; return; }
    if (key_active) { release_all_keys(); key_timer = key_gap_frames; return; }
    if (kq_count == 0) return;

    KeyEvent ev = key_queue[kq_head];
    kq_head = (kq_head + 1) % KEY_QUEUE_SIZE;
    kq_count--;

    if (ev.pressed) {
        if (ev.shift) hal_keyboard_press(SHIFT_ROW, SHIFT_COL);
        hal_keyboard_press(ev.row, ev.col);
        key_active = true;
        key_timer = key_hold_frames;
    } else {
        hal_keyboard_release(ev.row, ev.col);
        if (ev.shift) hal_keyboard_release(SHIFT_ROW, SHIFT_COL);
    }
}

void IntegrationTest::drain_key_queue(int settle_frames) {
    while (kq_count > 0 || key_active || key_timer > 0) run_frames(1);
    if (settle_frames > 0) run_frames(settle_frames);
}

void IntegrationTest::inject_keystrokes(const char* text, int delay_frames) {
    key_hold_frames = delay_frames > 0 ? delay_frames : 3;

    Serial.printf("  [kbd] inject: \"");
    for (const char* p = text; *p; p++)
        Serial.printf("%c", (*p >= 0x20 && *p < 0x7F) ? *p : '.');
    Serial.printf("\"\n");

    while (*text) {
        uint8_t row, col;
        bool shift;
        if (ascii_to_matrix(*text, row, col, shift)) {
            queue_key_event(row, col, shift, true);
            queue_key_event(row, col, shift, false);
        }
        text++;
    }
}

// ============================================================================
// Screen text helpers
// ============================================================================

void IntegrationTest::capture_screen_text(char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    int count = SCREEN_SIZE;
    if (count >= (int)buf_size) count = (int)buf_size - 1;
    for (int i = 0; i < count; i++)
        buf[i] = vram_to_ascii(machine->ram[TEXT_SCREEN_ADDR + i]);
    buf[count] = '\0';
}

bool IntegrationTest::screen_contains(const char* text) {
    char screen[SCREEN_SIZE + 1];
    capture_screen_text(screen, sizeof(screen));
    return strstr(screen, text) != nullptr;
}

bool IntegrationTest::wait_for_screen_text(const char* text, int timeout_frames) {
    for (int f = 0; f < timeout_frames; f++) {
        if (screen_contains(text)) return true;
        run_frames(1);
    }
    return false;
}

void IntegrationTest::capture_vram_snapshot(uint8_t* out, size_t max_bytes) {
    if (!out || max_bytes == 0) return;
    size_t len = (max_bytes < (size_t)SCREEN_SIZE) ? max_bytes : (size_t)SCREEN_SIZE;
    memcpy(out, &machine->ram[TEXT_SCREEN_ADDR], len);
}

bool IntegrationTest::compare_vram_region(uint16_t offset, const uint8_t* expected, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (machine->ram[TEXT_SCREEN_ADDR + offset + i] != expected[i])
            return false;
    }
    return true;
}

// ============================================================================
// VRAM diagnostics
// ============================================================================

void IntegrationTest::dump_vram_hex(int rows) {
    if (rows > SCREEN_ROWS) rows = SCREEN_ROWS;
    Serial.println("\n--- VRAM Hex Dump ($0400) ---");
    Serial.println("       +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F  ASCII");
    for (int r = 0; r < rows; r++) {
        uint16_t addr = TEXT_SCREEN_ADDR + r * SCREEN_COLS;
        for (int half = 0; half < 2; half++) {
            uint16_t la = addr + half * 16;
            Serial.printf("$%04X: ", la);
            char ascii[17];
            for (int i = 0; i < 16; i++) {
                uint8_t b = machine->ram[la + i];
                Serial.printf("%02X ", b);
                ascii[i] = vram_to_ascii(b);
                if (ascii[i] < ' ' || ascii[i] > '~') ascii[i] = '.';
            }
            ascii[16] = '\0';
            Serial.printf(" %s\n", ascii);
        }
    }
    Serial.println("----------------------------");
}

void IntegrationTest::dump_screen_text() {
    Serial.println("\n--- Screen Text ---");
    Serial.println("+--------------------------------+");
    char line[SCREEN_COLS + 1];
    for (int r = 0; r < SCREEN_ROWS; r++) {
        for (int c = 0; c < SCREEN_COLS; c++) {
            line[c] = vram_to_ascii(machine->ram[TEXT_SCREEN_ADDR + r * SCREEN_COLS + c]);
            if (line[c] < ' ' || line[c] > '~') line[c] = '.';
        }
        line[SCREEN_COLS] = '\0';
        Serial.printf("|%s|\n", line);
    }
    Serial.println("+--------------------------------+");
}

// ============================================================================
// Ensure booted helper
// ============================================================================

bool IntegrationTest::ensure_booted() {
    if (boot_verified && screen_contains("OK")) {
        release_all_keys();
        for (int i = 0; i < 8; i++)
            machine->ram[0x0152 + i] = 0xFF;
        run_frames(15);
        return true;
    }
    machine_reset(machine);
    release_all_keys();
    frame_counter = 0;
    if (wait_for_screen_text("OK", 360)) {
        boot_verified = true;
        run_frames(15);
        return true;
    }
    return false;
}

// ============================================================================
// Result recording
// ============================================================================

void IntegrationTest::record(const char* name, bool passed, uint32_t frames, uint32_t ms) {
    if (result_count < MAX_TESTS) {
        results[result_count].name = name;
        results[result_count].passed = passed;
        results[result_count].elapsed_frames = frames;
        results[result_count].elapsed_ms = ms;
        result_count++;
    }
    if (passed) pass_count++; else fail_count++;
}

// ============================================================================
// RS-DOS disk helpers: read a file from disk cache
// ============================================================================

// Read a named file from disk cache by following RS-DOS directory + granule chain.
// Allocates *file_buf with malloc (caller must free). Returns file size or -1.
int IntegrationTest::read_rsdos_file(const char* filename, uint8_t** file_buf, int drive) {
    SV_DiskImage* disk = &machine->fdc.drives[drive];
    if (!disk->mounted || !disk->cache) {
        Serial.printf("  [disk] Drive %d not mounted or no cache\n", drive);
        return -1;
    }

    uint16_t sec_size = disk->sector_size;
    uint8_t spt = disk->sectors_per_track;

    // Helper lambda: get pointer to sector data in cache
    // Track/sector are 0-based track, 1-based sector
    auto sector_ptr = [&](int track, int sector) -> uint8_t* {
        if (sector < 1 || sector > spt || track < 0 || track >= disk->tracks)
            return nullptr;
        uint32_t off = (uint32_t)track * spt * sec_size + (uint32_t)(sector - 1) * sec_size;
        if (off + sec_size > disk->cache_size) return nullptr;
        return disk->cache + off;
    };

    // Read FAT (track 17, sector 2)
    uint8_t* fat = sector_ptr(RSDOS_DIR_TRACK, RSDOS_FAT_SECTOR);
    if (!fat) {
        Serial.println("  [disk] Cannot read FAT sector");
        return -1;
    }

    // Build padded 8.3 filename for comparison (RS-DOS: 8-char name + 3-char ext, space-padded)
    char name83[12]; // 8 + 3 + NUL
    memset(name83, ' ', 11);
    name83[11] = '\0';

    // Parse "FILENAME" or "FILENAME.EXT"
    const char* dot = strchr(filename, '.');
    int name_len = dot ? (int)(dot - filename) : (int)strlen(filename);
    if (name_len > 8) name_len = 8;
    for (int i = 0; i < name_len; i++)
        name83[i] = toupper(filename[i]);

    if (dot) {
        const char* ext = dot + 1;
        int ext_len = strlen(ext);
        if (ext_len > 3) ext_len = 3;
        for (int i = 0; i < ext_len; i++)
            name83[8 + i] = toupper(ext[i]);
    }

    Serial.printf("  [disk] Searching for '%.8s.%.3s' in directory\n", name83, name83 + 8);

    // Scan directory (track 17, sectors 3-11)
    int first_granule = -1;
    int file_type = -1;
    int ascii_flag = -1;

    for (int sec = RSDOS_DIR_FIRST_SEC; sec <= RSDOS_DIR_LAST_SEC; sec++) {
        uint8_t* dir = sector_ptr(RSDOS_DIR_TRACK, sec);
        if (!dir) continue;

        for (int e = 0; e < RSDOS_ENTRIES_PER_SEC; e++) {
            uint8_t* entry = dir + e * RSDOS_DIR_ENTRY_SIZE;

            // Byte 0: $00 = deleted, $FF = end of directory
            if (entry[0] == 0xFF) goto dir_done;
            if (entry[0] == 0x00) continue;

            // Compare 8+3 name (bytes 0-7 = name, 8-10 = extension)
            if (memcmp(entry, name83, 11) == 0) {
                file_type = entry[11];     // 0=BASIC, 1=data, 2=ML
                ascii_flag = entry[12];    // 0=binary, $FF=ASCII
                first_granule = entry[13]; // First granule number
                Serial.printf("  [disk] Found: type=%d ascii=%d first_gran=%d\n",
                              file_type, ascii_flag, first_granule);
                goto dir_done;
            }
        }
    }
dir_done:

    if (first_granule < 0) {
        Serial.printf("  [disk] File '%s' not found in directory\n", filename);
        return -1;
    }

    // Follow granule chain to collect all sectors
    // Granule N maps to: track = N/2, starting sector = (N%2)*9 + 1
    // FAT[N]: $C0-$C9 = last granule, value & 0x0F = number of sectors used in last granule
    //         $00-$43 = next granule number

    // First pass: count total bytes
    int total_bytes = 0;
    int gran = first_granule;
    int chain_len = 0;
    const int MAX_CHAIN = 128; // safety limit

    // Temporary chain storage
    struct GranInfo { int granule; int sectors; };
    GranInfo chain[MAX_CHAIN];

    while (chain_len < MAX_CHAIN) {
        if (gran < 0 || gran > 67) {
            Serial.printf("  [disk] Invalid granule %d in chain\n", gran);
            return -1;
        }

        uint8_t fat_entry = fat[gran];

        if (fat_entry >= 0xC0 && fat_entry <= 0xC9) {
            // Last granule: low nibble = sectors used (1-9)
            int secs = fat_entry & 0x0F;
            if (secs == 0) secs = 9; // $C0 means 9 sectors? Actually 0 means read last sector bytes from dir
            // RS-DOS: bytes in last sector from directory entry bytes 16-17
            chain[chain_len].granule = gran;
            chain[chain_len].sectors = secs;
            chain_len++;
            total_bytes += secs * sec_size;
            break;
        } else {
            // Full granule (9 sectors)
            chain[chain_len].granule = gran;
            chain[chain_len].sectors = RSDOS_GRANULE_SECTORS;
            chain_len++;
            total_bytes += RSDOS_GRANULE_SECTORS * sec_size;
            gran = fat_entry;
        }
    }

    // The last granule's last sector may not be fully used.
    // RS-DOS directory entry bytes 14-15 (big-endian) = bytes used in last sector of last granule
    // We need to re-scan directory for this — but for LOADM comparison we'll
    // use the LOADM preamble to determine exact lengths. So read full sectors.

    Serial.printf("  [disk] Granule chain: %d granules, %d bytes (full sectors)\n",
                  chain_len, total_bytes);

    if (total_bytes == 0) {
        Serial.println("  [disk] File is empty");
        return -1;
    }

    // Allocate buffer and read sectors
    uint8_t* buf = (uint8_t*)malloc(total_bytes);
    if (!buf) {
        Serial.printf("  [disk] malloc(%d) failed\n", total_bytes);
        return -1;
    }

    int offset = 0;
    for (int ci = 0; ci < chain_len; ci++) {
        int g = chain[ci].granule;
        int nsecs = chain[ci].sectors;
        int track = g / 2;
        if (track >= RSDOS_DIR_TRACK) track++;  // skip directory track 17
        int start_sector = (g % 2) * RSDOS_GRANULE_SECTORS + 1;

        for (int s = 0; s < nsecs; s++) {
            uint8_t* sp = sector_ptr(track, start_sector + s);
            if (!sp) {
                Serial.printf("  [disk] Bad sector: T%d S%d\n", track, start_sector + s);
                free(buf);
                return -1;
            }
            memcpy(buf + offset, sp, sec_size);
            offset += sec_size;
        }
    }

    Serial.printf("  [disk] Read %d bytes from disk cache\n", offset);
    *file_buf = buf;
    return offset;
}

// ============================================================================
// Parse LOADM preamble/postamble
// ============================================================================

int IntegrationTest::parse_loadm_segments(const uint8_t* data, int size,
                                           LoadmSegment* segs, int max_segs,
                                           uint16_t* exec_addr) {
    int pos = 0;
    int seg_count = 0;
    *exec_addr = 0;

    while (pos < size && seg_count < max_segs) {
        if (pos + 5 > size) break;

        uint8_t type = data[pos];

        if (type == LOADM_PREAMBLE) {
            // Preamble: $00 len_hi len_lo addr_hi addr_lo data[len]
            uint16_t len  = ((uint16_t)data[pos + 1] << 8) | data[pos + 2];
            uint16_t addr = ((uint16_t)data[pos + 3] << 8) | data[pos + 4];
            pos += 5;

            if (len == 0) {
                // Zero-length preamble = skip (sometimes used as padding)
                continue;
            }

            segs[seg_count].addr = addr;
            segs[seg_count].length = len;
            segs[seg_count].file_off = pos;
            seg_count++;

            Serial.printf("  [loadm] Segment %d: $%04X len=%d (file offset %d)\n",
                          seg_count, addr, len, pos);
            pos += len;
        } else if (type == LOADM_POSTAMBLE) {
            // Postamble: $FF $00 $00 exec_hi exec_lo
            *exec_addr = ((uint16_t)data[pos + 3] << 8) | data[pos + 4];
            Serial.printf("  [loadm] Exec addr: $%04X\n", *exec_addr);
            break;
        } else {
            Serial.printf("  [loadm] Unexpected byte $%02X at offset %d\n", type, pos);
            break;
        }
    }

    return seg_count;
}

// ============================================================================
// Read a raw file from SD card. Allocates buffer with malloc (caller frees).
// Returns file size or -1 on error.
// ============================================================================

int IntegrationTest::read_sd_file(const char* path, uint8_t** out_buf) {
    if (!SD.exists(path)) {
        Serial.printf("  [sd] File '%s' not found on SD card\n", path);
        return -1;
    }
    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf("  [sd] Failed to open '%s'\n", path);
        return -1;
    }
    int sz = (int)f.size();
    if (sz <= 0) {
        f.close();
        Serial.printf("  [sd] File '%s' is empty\n", path);
        return -1;
    }
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) {
        f.close();
        Serial.printf("  [sd] malloc(%d) failed\n", sz);
        return -1;
    }
    int rd = (int)f.read(buf, sz);
    f.close();
    if (rd != sz) {
        Serial.printf("  [sd] Read %d of %d bytes\n", rd, sz);
        free(buf);
        return -1;
    }
    Serial.printf("  [sd] Read %d bytes from '%s'\n", sz, path);
    *out_buf = buf;
    return sz;
}

// ============================================================================
// Compare two buffers, report matches/mismatches with hex dump
// ============================================================================

void IntegrationTest::compare_buffers(const char* label,
                                       const uint8_t* expected, const uint8_t* actual,
                                       int len, int base_addr) {
    int matches = 0, mismatches = 0;
    int first_mismatch = -1;

    for (int i = 0; i < len; i++) {
        if (expected[i] == actual[i]) {
            matches++;
        } else {
            if (mismatches < 16) {
                Serial.printf("    MISMATCH @$%04X: expected=$%02X got=$%02X (offset %d)\n",
                              base_addr + i, expected[i], actual[i], i);
            }
            if (first_mismatch < 0) first_mismatch = i;
            mismatches++;
        }
    }

    Serial.printf("  [%s] %d bytes compared: %d match, %d mismatch",
                  label, len, matches, mismatches);
    if (base_addr >= 0)
        Serial.printf(" (base addr $%04X)", base_addr);
    Serial.println();

    // Hex dump around first mismatch
    if (first_mismatch >= 0) {
        int dstart = (first_mismatch > 16) ? first_mismatch - 16 : 0;
        int dend = first_mismatch + 32;
        if (dend > len) dend = len;

        Serial.printf("    Expected (offset %d):\n    ", dstart);
        for (int i = dstart; i < dend; i++)
            Serial.printf("%02X ", expected[i]);
        Serial.println();

        Serial.printf("    Actual   (offset %d):\n    ", dstart);
        for (int i = dstart; i < dend; i++)
            Serial.printf("%02X ", actual[i]);
        Serial.println();
    }
}

// ============================================================================
// Test: LOADM verification
// ============================================================================

// User-triggered verification: user types LOADM on the CoCo keyboard,
// then sends 'R' on serial to verify RAM against disk cache.
//
// Three-way comparison:
// 1. SD card file vs disk image extraction (validates our RS-DOS reader)
// 2. File data (after LOADM headers) vs CoCo RAM (validates the load)
bool IntegrationTest::test_loadm_verify(const char* filename, int iterations) {
    Serial.println("\n========================================");
    Serial.println("=== LOADM Verify Test ===");
    Serial.println("========================================");
    Serial.printf("  File: %s\n", filename);

    // --- Step 1: Read raw file from SD card ---
    char sd_path[64];
    snprintf(sd_path, sizeof(sd_path), "/%s", filename);
    uint8_t* sd_data = nullptr;
    int sd_size = read_sd_file(sd_path, &sd_data);

    // --- Step 2: Read file from disk image cache ---
    uint8_t* disk_data = nullptr;
    int disk_size = read_rsdos_file(filename, &disk_data, 0);

    // --- Step 3: Compare SD vs disk extraction ---
    if (sd_data && sd_size > 0 && disk_data && disk_size > 0) {
        Serial.println("\n--- SD Card vs Disk Image Extraction ---");
        Serial.printf("  SD card file:   %d bytes\n", sd_size);
        Serial.printf("  Disk extracted: %d bytes\n", disk_size);

        int cmp_len = (sd_size < disk_size) ? sd_size : disk_size;
        compare_buffers("SD vs Disk", sd_data, disk_data, cmp_len, -1);

        if (sd_size != disk_size) {
            Serial.printf("  WARNING: Size mismatch! SD=%d, Disk=%d (diff=%d)\n",
                          sd_size, disk_size, disk_size - sd_size);
            Serial.println("  Note: Disk extraction reads full sectors, SD file is exact size.");
            Serial.println("  The disk may have extra padding bytes at the end.");
        }
    } else {
        if (!sd_data || sd_size <= 0)
            Serial.printf("  [INFO] No SD card file at '%s' — skipping SD comparison\n", sd_path);
        if (!disk_data || disk_size <= 0) {
            Serial.println("  [FAIL] Could not read file from disk cache");
            if (sd_data) free(sd_data);
            return false;
        }
    }

    // --- Step 4: Parse LOADM segments from the file ---
    // Use SD file if available (exact size), otherwise disk extraction
    uint8_t* file_data = sd_data ? sd_data : disk_data;
    int file_size = sd_data ? sd_size : disk_size;

    Serial.println("\n--- LOADM Header Parsing ---");
    Serial.printf("  First 16 bytes of file: ");
    for (int i = 0; i < 16 && i < file_size; i++)
        Serial.printf("%02X ", file_data[i]);
    Serial.println();

    LoadmSegment segs[MAX_LOADM_SEGMENTS];
    uint16_t exec_addr = 0;
    int seg_count = parse_loadm_segments(file_data, file_size, segs, MAX_LOADM_SEGMENTS, &exec_addr);

    if (seg_count == 0) {
        Serial.println("  [FAIL] No valid LOADM segments found");
        if (sd_data) free(sd_data);
        if (disk_data) free(disk_data);
        return false;
    }

    uint32_t total_data_bytes = 0;
    for (int s = 0; s < seg_count; s++)
        total_data_bytes += segs[s].length;

    Serial.printf("  Segments: %d, total data payload: %lu bytes\n",
                  seg_count, (unsigned long)total_data_bytes);
    Serial.printf("  Exec address: $%04X\n", exec_addr);

    // --- Step 5: Compare each LOADM segment data vs CoCo RAM ---
    Serial.println("\n--- CoCo RAM Verification ---");
    Serial.println("  (Comparing LOADM data payload vs CoCo RAM at load addresses)");
    dump_screen_text();

    bool all_pass = true;
    int total_matches = 0;
    int total_mismatches = 0;

    for (int s = 0; s < seg_count; s++) {
        uint16_t addr = segs[s].addr;
        uint16_t len  = segs[s].length;
        uint32_t foff = segs[s].file_off;  // offset past the 5-byte header

        Serial.printf("\n  Segment %d: load addr=$%04X, length=%d, file_offset=%d\n",
                      s, addr, len, (int)foff);
        Serial.printf("  Header skipped: bytes 0-%d (type=$%02X size=$%04X addr=$%04X)\n",
                      (int)(foff - 1), file_data[foff - 5],
                      (uint16_t)((file_data[foff - 4] << 8) | file_data[foff - 3]),
                      (uint16_t)((file_data[foff - 2] << 8) | file_data[foff - 1]));

        if (addr + len > 0x8000) {
            Serial.printf("  [seg%d] $%04X+%d overlaps ROM area, skipping\n", s, addr, len);
            continue;
        }

        int seg_matches = 0, seg_mismatches = 0;
        int first_mismatch = -1;

        for (int i = 0; i < len; i++) {
            uint8_t expected = file_data[foff + i];
            uint8_t actual   = machine->ram[addr + i];
            if (expected == actual) {
                seg_matches++;
            } else {
                if (seg_mismatches < 16) {
                    Serial.printf("    MISMATCH @$%04X: expected=$%02X got=$%02X (file+%d)\n",
                                  addr + i, expected, actual, (int)(foff + i));
                }
                if (first_mismatch < 0) first_mismatch = i;
                seg_mismatches++;
            }
        }

        Serial.printf("  [seg%d] CoCo addr $%04X-%04X: %d match, %d mismatch out of %d bytes\n",
                      s, addr, addr + len - 1, seg_matches, seg_mismatches, len);

        total_matches += seg_matches;
        total_mismatches += seg_mismatches;

        if (seg_mismatches > 0) {
            all_pass = false;

            // Hex dump around first mismatch
            if (first_mismatch >= 0) {
                int dstart = (first_mismatch > 16) ? first_mismatch - 16 : 0;
                int dend = first_mismatch + 32;
                if (dend > len) dend = len;

                Serial.printf("    Expected (file offset %d):\n    ", (int)(foff + dstart));
                for (int i = dstart; i < dend; i++)
                    Serial.printf("%02X ", file_data[foff + i]);
                Serial.println();

                Serial.printf("    Actual   (RAM $%04X):\n    ", addr + dstart);
                for (int i = dstart; i < dend; i++)
                    Serial.printf("%02X ", machine->ram[addr + i]);
                Serial.println();
            }
        }
    }

    // --- Summary ---
    Serial.println("\n========================================");
    Serial.printf("  TOTAL: %d match + %d mismatch = %d bytes checked\n",
                  total_matches, total_mismatches, total_matches + total_mismatches);
    if (all_pass) {
        Serial.printf("  RESULT: PASS — all %d bytes verified OK\n", total_matches);
    } else {
        Serial.printf("  RESULT: FAIL — %d mismatches found\n", total_mismatches);
    }
    Serial.println("========================================\n");

    if (sd_data) free(sd_data);
    if (disk_data) free(disk_data);

    return all_pass;
}

// ============================================================================
// Run all (dispatches single verify)
// ============================================================================

void IntegrationTest::run_all(bool stop_on_failure) {
    pass_count = 0;
    fail_count = 0;
    result_count = 0;

    uint32_t sm = millis();
    bool p = test_loadm_verify("ZAXXON.BIN", 1);
    uint32_t em = millis() - sm;

    record("LOADM ZAXXON.BIN", p, 0, em);

    Serial.printf("=== %s (%u ms) ===\n", p ? "PASS" : "FAIL", em);
}

// ============================================================================
// Print report
// ============================================================================

void IntegrationTest::print_report() {
    if (result_count == 0) {
        Serial.println("No test results. Run tests first (R).");
        return;
    }
    Serial.println("\n=== Last Test Report ===");
    for (int i = 0; i < result_count; i++) {
        Serial.printf("  [%s] %-22s %5u frames  %5u ms\n",
                      results[i].passed ? "PASS" : "FAIL",
                      results[i].name,
                      results[i].elapsed_frames,
                      results[i].elapsed_ms);
    }
    Serial.printf("Total: %d PASS  %d FAIL\n", pass_count, fail_count);
}

// ============================================================================
// Serial command interface
// ============================================================================

void IntegrationTest::process_serial_command(char cmd) {
    switch (cmd) {
        case 'R': case 'r':
            run_all(false);
            break;

        case 'S': case 's':
            print_report();
            break;

        case 'D': case 'd':
            dump_vram_hex(16);
            break;

        case 'T': case 't':
            dump_screen_text();
            break;
    }
}

/* ==========================================================================
 * Commented-out old tests (kept for reference):
 *
 * bool IntegrationTest::test_boot_sequence()   — boot to OK prompt
 * bool IntegrationTest::test_basic_print()     — PRINT "ABCDEFGH"
 * bool IntegrationTest::test_basic_for_loop()  — FOR I=1 TO 10
 * bool IntegrationTest::test_graphics_pmode4() — PMODE 3 page flip
 * bool IntegrationTest::test_sound_output()    — SOUND 100,10
 *
 * These were in the original integration_test.cpp and can be restored
 * by uncommenting their declarations in integration_test.h and re-adding
 * the implementations from git history.
 * ========================================================================== */
