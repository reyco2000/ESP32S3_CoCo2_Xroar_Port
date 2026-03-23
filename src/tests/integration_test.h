/*
 * ============================================================
 *   CoCo_ESP32 Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : integration_test.h
 *  Module : Integration test framework interface — LOADM, VRAM dump, and serial test commands
 * ============================================================
 */

/*
 * CoCo ESP32 - Integration Test Framework
 *
 * LOADM disk verification test: loads a binary from disk via LOADM,
 * then compares CoCo RAM with the raw file data from the disk cache.
 *
 * Serial commands:
 *   'R' - run LOADM verify test (N iterations)
 *   'S' - print last report
 *   'D' - dump VRAM hex
 *   'T' - dump screen as text
 */

#ifndef INTEGRATION_TEST_H
#define INTEGRATION_TEST_H

#include <stdint.h>
#include <stddef.h>
#include "../core/machine.h"

// RS-DOS constants
#define RSDOS_DIR_TRACK      17
#define RSDOS_DIR_FIRST_SEC  3     // Directory: sectors 3-11
#define RSDOS_DIR_LAST_SEC   11
#define RSDOS_FAT_SECTOR     2     // FAT (granule allocation table): track 17, sector 2
#define RSDOS_GRANULE_SECTORS 9    // 9 sectors per granule (half-track)
#define RSDOS_DIR_ENTRY_SIZE 32
#define RSDOS_ENTRIES_PER_SEC (256 / RSDOS_DIR_ENTRY_SIZE)  // 8

// LOADM preamble/postamble
#define LOADM_PREAMBLE  0x00
#define LOADM_POSTAMBLE 0xFF

// Max segments in a LOADM binary
#define MAX_LOADM_SEGMENTS 16

class IntegrationTest {
public:
    explicit IntegrationTest(Machine* m);

    // --- Active test ---
    bool test_loadm_verify(const char* filename, int iterations = 5);

    // --- Helpers ---
    void inject_keystrokes(const char* text, int delay_frames = 3);
    bool wait_for_screen_text(const char* text, int timeout_frames = 360);
    void capture_vram_snapshot(uint8_t* out, size_t max_bytes);
    bool compare_vram_region(uint16_t offset, const uint8_t* expected, size_t len);

    // --- VRAM diagnostics ---
    void dump_vram_hex(int rows = 16);
    void dump_screen_text();

    // --- Run / report ---
    void run_all(bool stop_on_failure = false);
    void print_report();

    // --- Serial command interface ---
    void process_serial_command(char cmd);

    // --- Headless mode ---
    void set_headless(bool enabled) { headless = enabled; }
    bool is_headless() const { return headless; }

    /* ---- Commented-out old tests (kept for reference) ----
    bool test_boot_sequence();
    bool test_basic_print();
    bool test_basic_for_loop();
    bool test_graphics_pmode4();
    bool test_sound_output();
    -------------------------------------------------------- */

private:
    Machine* machine;
    int pass_count = 0;
    int fail_count = 0;
    bool headless = true;

    void run_frames(int count);

    struct TestResult {
        const char* name;
        bool passed;
        uint32_t elapsed_frames;
        uint32_t elapsed_ms;
    };
    static const int MAX_TESTS = 16;
    TestResult results[MAX_TESTS];
    int result_count = 0;

    void record(const char* name, bool passed, uint32_t frames, uint32_t ms);

    // --- Keyboard injection internals ---
    struct KeyEvent {
        uint8_t row;
        uint8_t col;
        bool shift;
        bool pressed;
    };
    static const int KEY_QUEUE_SIZE = 512;
    KeyEvent key_queue[KEY_QUEUE_SIZE];
    int kq_head = 0;
    int kq_tail = 0;
    int kq_count = 0;

    int key_hold_frames = 3;
    int key_gap_frames = 2;
    int key_timer = 0;
    bool key_active = false;

    void queue_key_event(uint8_t row, uint8_t col, bool shift, bool pressed);
    void process_key_queue();
    void release_all_keys();
    void drain_key_queue(int settle_frames = 30);

    static bool ascii_to_matrix(char c, uint8_t& row, uint8_t& col, bool& shift_out);
    static char vram_to_ascii(uint8_t vram_byte);

    void capture_screen_text(char* buf, size_t buf_size);
    bool screen_contains(const char* text);
    bool ensure_booted();

    // --- Disk image helpers for LOADM verification ---
    struct LoadmSegment {
        uint16_t addr;      // Load address
        uint16_t length;    // Byte count
        uint32_t file_off;  // Offset within the raw file data
    };

    // Read raw file bytes from disk cache by following RS-DOS directory + FAT
    // Returns total file size, fills file_buf (caller must free with free())
    int read_rsdos_file(const char* filename, uint8_t** file_buf, int drive = 0);

    // Read raw file from SD card. Returns size, fills out_buf (caller frees).
    int read_sd_file(const char* path, uint8_t** out_buf);

    // Compare two buffers and report matches/mismatches with hex dump.
    // base_addr: display address for reporting (-1 to omit).
    void compare_buffers(const char* label, const uint8_t* expected,
                         const uint8_t* actual, int len, int base_addr);

    // Parse LOADM preamble/postamble segments from raw file data
    int parse_loadm_segments(const uint8_t* file_data, int file_size,
                             LoadmSegment* segs, int max_segs,
                             uint16_t* exec_addr);

    uint32_t frame_counter = 0;
    bool boot_verified = false;
};

#endif // INTEGRATION_TEST_H
