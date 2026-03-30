#pragma GCC optimize("O2")
/*
 * ============================================================
 *   CoCo_ESP32 Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : machine.cpp
 *  Module : CoCo 2 machine integration — wires CPU, PIAs, VDG, SAM, and memory map
 * ============================================================
 */

/*
 * machine.cpp - CoCo/Dragon system integration
 *
 * Wires all emulation modules together:
 *   CPU ↔ SAM address decoding ↔ RAM/ROM/PIA/SAM registers
 *   PIA0 ← keyboard matrix (via HAL) + joystick comparator
 *   PIA1 → VDG mode bits + single-bit audio
 *   VDG  ← VRAM (via SAM display offset)
 *   IRQ routing: PIA0 → FIRQ, PIA1 → IRQ
 *
 * CoCo 2 memory map (64K, SAM mem_size=2):
 *   $0000-$7FFF  RAM (lower 32K, or full 64K with SAM page select)
 *   $8000-$9FFF  Extended BASIC ROM (8K)
 *   $A000-$BFFF  Color BASIC ROM (8K)
 *   $C000-$FEFF  Cartridge ROM / Disk BASIC (16K, optional)
 *   $FF00-$FF1F  PIA0 (4 regs at $FF00-$FF03, mirrored)
 *   $FF20-$FF3F  PIA1 (4 regs at $FF20-$FF23, mirrored)
 *   $FF40-$FFBF  Disk controller / reserved
 *   $FFC0-$FFDF  SAM control bits (write-only, bit set/clear pairs)
 *   $FFE0-$FFFF  Vectors (read from ROM)
 */

#include "machine.h"
#include "../hal/hal.h"
#include "../utils/debug.h"

// Global machine pointer for CPU memory callbacks
static Machine* g_machine = nullptr;

// Update VDG mode from PIA1 (AG, CSS) + SAM (GM0-GM2)
static void update_vdg_mode(Machine* m) {
    uint8_t pb = m->pia1.data_b & m->pia1.ddr_b;
    uint8_t vdg_mode = 0;
    // AG from PIA1 PB7
    if (pb & 0x80) vdg_mode |= VDG_AG;
    // CSS from PIA1 PB3
    if (pb & 0x08) vdg_mode |= VDG_CSS;
    // GM0-GM2 from SAM V0-V2
    vdg_mode |= (m->sam.vdg_mode & 0x07);  // GM0=bit0, GM1=bit1, GM2=bit2
    mc6847_set_mode(&m->vdg, vdg_mode);
}

// Cycles per scanline: CPU_CLOCK_HZ / TARGET_FPS / SCANLINES_PER_FRAME
// 895000 / 60 / 262 ≈ 56.9 → use fixed-point for accuracy
static const int CYCLES_PER_SCANLINE_X4 = (CPU_CLOCK_HZ * 4) / (TARGET_FPS * SCANLINES_PER_FRAME);
// That gives ~228 (57.0 * 4), we alternate 57 and 57 cycles

// ============================================================
// IRQ routing callbacks
// ============================================================

// CoCo IRQ routing (verified from BASIC ROM vectors):
//   IRQ vector ($FFF8) → $010C (RAM) → XIRQSV handles 60Hz timer
//   FIRQ vector ($FFF6) → $010F (RAM) → cartridge/fast interrupt
//
// PIA0 IRQA/IRQB → CPU IRQ (60Hz vsync timer, keyboard hsync)
static void pia0_irq_a_callback(bool active) {
    if (g_machine) mc6809_irq(&g_machine->cpu, active);
}

static void pia0_irq_b_callback(bool active) {
    if (g_machine) mc6809_irq(&g_machine->cpu, active);
}

// PIA1 IRQA/IRQB → CPU FIRQ (cartridge)
static void pia1_irq_a_callback(bool active) {
    if (g_machine) mc6809_firq(&g_machine->cpu, active);
}

static void pia1_irq_b_callback(bool active) {
    if (g_machine) mc6809_firq(&g_machine->cpu, active);
}

// ============================================================
// Memory read callback (called by CPU)
// ============================================================

uint8_t machine_read(uint16_t addr) {
    Machine* m = g_machine;
    if (!m) return 0xFF;

    // --- I/O space: $FF00-$FFFF ---
    if (addr >= 0xFF00) {
        // PIA0: $FF00-$FF1F (4 registers mirrored every 4 bytes)
        if (addr < 0xFF20) {
            uint8_t reg = addr & 0x03;

            // Special handling for PIA0 port A read: scan keyboard matrix + joystick
            // Before reading, update input_a based on which columns port B selects
            if (reg == PIA_REG_DA && (m->pia0.ctrl_a & PIA_CR_DDR_SEL)) {
                // PIA0 port B drives column select (active low)
                uint8_t col_select = m->pia0.data_b & m->pia0.ddr_b;
                uint8_t row_data = 0xFF;

                for (int col = 0; col < 8; col++) {
                    if (!(col_select & (1 << col))) {
                        // Column driven low → scan this column
                        row_data &= hal_keyboard_scan(col);
                    }
                }

                // Joystick buttons: right button → PA0 low, left button → PA1 low
                if (hal_joystick_read_button(0))
                    row_data &= ~0x01;
                if (hal_joystick_read_button(1))
                    row_data &= ~0x02;

                // Joystick comparator on PA7 (matches XRoar joystick_update exactly):
                //   port = PIA0 CRB bit 3 (CB2 output select)
                //   axis = PIA0 CRA bit 3 (CA2 output select)
                //   dac  = (PIA1 DA & 0xFC) + 2  (8-bit, range 2-254)
                //   js   = axis_6bit * 4 + 2      (scale 0-63 → 2-254)
                //
                // Refresh ADC once per scanline (not every PIA read) so asm
                // programs polling in tight loops see updated joystick values.
                {
                    // Refresh ADC every 16 scanlines (~16 updates/frame).
                    // Enough for responsive input without excessive ADC overhead.
                    static uint32_t last_joy_scanline = UINT32_MAX;
                    uint32_t joy_slot = m->scanline >> 4;  // divide by 16
                    if (joy_slot != last_joy_scanline) {
                        last_joy_scanline = joy_slot;
                        hal_joystick_update();
                    }
                    int joy_port = (m->pia0.ctrl_b & 0x08) >> 3;
                    int joy_axis = (m->pia0.ctrl_a & 0x08) >> 3;
                    int dac_value = ((m->pia1.data_a & m->pia1.ddr_a) & 0xFC) + 2;
                    int js_value = hal_joystick_read_axis(joy_port, joy_axis) * 4 + 2;
                    if (js_value >= dac_value)
                        row_data |= 0x80;
                    else
                        row_data &= ~0x80;
                }

                mc6821_set_input_a(&m->pia0, row_data);
            }

            return mc6821_read(&m->pia0, reg);
        }

        // PIA1: $FF20-$FF3F (4 registers mirrored every 4 bytes)
        if (addr < 0xFF40) {
            return mc6821_read(&m->pia1, addr & 0x03);
        }

        // Disk controller: $FF40-$FF5F (WD1793 FDC)
        if (addr < 0xFF60) {
            return sv_disk_read(&m->fdc, addr);
        }

        // Reserved: $FF60-$FFBF
        if (addr < 0xFFC0) {
            return 0xFF;
        }

        // SAM: $FFC0-$FFDF (write-only, reads return open bus)
        if (addr < 0xFFE0) {
            return 0xFF;
        }

        // Vectors: $FFE0-$FFFF — read from ROM (end of Extended BASIC)
        // These map to $FFE0-$FFFF which is in the $8000-$9FFF region offset
        // Vectors are at the top of the address space in ROM
        if (m->rom_extbas_loaded) {
            // ExtBASIC ROM covers $8000-$9FFF (8K)
            // Vector $FFFx maps to ExtBASIC offset ($FFFx - $8000) mod 8K
            // But vectors are actually at the end of a 16K block...
            // In a real CoCo, all ROM appears as a single 16K block $8000-$BFFF
            // mapped via SAM. The vectors at $FFF0-$FFFF mirror the top of ROM.
            // Color BASIC has vectors at its top ($BFFA-$BFFF)
            if (m->rom_basic_loaded) {
                // BASIC ROM is at $A000-$BFFF, vectors point into it
                uint16_t rom_offset = addr - 0xA000;
                if (rom_offset < 0x2000) {
                    return m->rom_basic[rom_offset];
                }
                // Fallthrough for addresses > $BFFF
                // Map $FFEx-$FFFx: use basic ROM's vector region
                // CoCo vectors at $FFF0-$FFFF correspond to BASIC ROM $BFF0-$BFFF
                rom_offset = 0x1FF0 + (addr & 0x0F);
                if (addr >= 0xFFF0) {
                    return m->rom_basic[rom_offset];
                }
                // $FFE0-$FFEF: also from basic ROM
                rom_offset = 0x1FE0 + (addr & 0x1F);
                return m->rom_basic[rom_offset];
            }
        }
        return 0xFF;
    }

    // --- ROM space: $8000-$FEFF ---
    // SAM MAP TYPE (ty) = 1: all-RAM mode — ROMs unmapped, full 64K is RAM
    if (addr >= 0x8000) {
        if (m->sam.ty && (size_t)addr < m->ram_size) {
            return m->ram[addr];
        }
        if (addr >= 0xC000) {
            // Cartridge ROM / Disk BASIC: $C000-$FEFF
            if (m->rom_cart_loaded) {
                return m->rom_cart[addr - 0xC000];
            }
            return 0xFF;
        }
        if (addr >= 0xA000) {
            // Color BASIC: $A000-$BFFF
            if (m->rom_basic_loaded) {
                return m->rom_basic[addr - 0xA000];
            }
            return 0xFF;
        }
        // Extended BASIC: $8000-$9FFF
        if (m->rom_extbas_loaded) {
            return m->rom_extbas[addr - 0x8000];
        }
        return 0xFF;
    }

    // --- RAM: $0000-$7FFF ---
    if ((size_t)addr < m->ram_size) {
        return m->ram[addr];
    }

    return 0xFF;
}

// ============================================================
// Memory write callback (called by CPU)
// ============================================================

void machine_write(uint16_t addr, uint8_t val) {
    Machine* m = g_machine;
    if (!m) return;

    // --- I/O space: $FF00-$FFFF ---
    if (addr >= 0xFF00) {
        // PIA0: $FF00-$FF1F
        if (addr < 0xFF20) {
            mc6821_write(&m->pia0, addr & 0x03, val);
            return;
        }

        // PIA1: $FF20-$FF3F
        if (addr < 0xFF40) {
            uint8_t reg = addr & 0x03;
            mc6821_write(&m->pia1, reg, val);

            // PIA1 port A: 6-bit DAC audio (bits 2-7)
            // Only output when sound MUX is enabled (PIA1 CRB bit 3) and
            // source is DAC (PIA0 CRA/CRB bit 3 both 0). BASIC clears
            // PIA1 CRB bit 3 during JOYSTK() to mute the DAC.
            if (reg == PIA_REG_DA || reg == PIA_REG_CRA) {
                bool mux_en = (m->pia1.ctrl_b & 0x08) != 0;
                uint8_t mux_src = ((m->pia0.ctrl_b & 0x08) >> 2)
                                | ((m->pia0.ctrl_a & 0x08) >> 3);
                if (mux_en && mux_src == 0) {
                    uint8_t pa = m->pia1.data_a & m->pia1.ddr_a;
                    hal_audio_write_dac((pa >> 2) & 0x3F);
                }
            }
            // PIA1 port B drives VDG AG (bit 7) and CSS (bit 3)
            // GM0-GM2 come from SAM V0-V2, not PIA1
            if (reg == PIA_REG_DB || reg == PIA_REG_CRB) {
                update_vdg_mode(m);
                // Single-bit audio: PIA1 port B bit 1 (independent of MUX)
                uint8_t pb = m->pia1.data_b & m->pia1.ddr_b;
                hal_audio_write_bit((pb & 0x02) != 0);
                // When MUX re-enabled with source=DAC, restore DAC audio
                if (reg == PIA_REG_CRB) {
                    bool mux_en = (m->pia1.ctrl_b & 0x08) != 0;
                    uint8_t mux_src = ((m->pia0.ctrl_b & 0x08) >> 2)
                                    | ((m->pia0.ctrl_a & 0x08) >> 3);
                    if (mux_en && mux_src == 0) {
                        uint8_t pa = m->pia1.data_a & m->pia1.ddr_a;
                        hal_audio_write_dac((pa >> 2) & 0x3F);
                    }
                }
            }
            return;
        }

        // Disk controller: $FF40-$FF5F (WD1793 FDC)
        if (addr < 0xFF60) {
            sv_disk_write(&m->fdc, addr, val);
            return;
        }

        // Reserved: $FF60-$FFBF
        if (addr < 0xFFC0) {
            return;
        }

        // SAM: $FFC0-$FFDF (write-only bit set/clear pairs)
        if (addr <= 0xFFDF) {
            sam6883_write(&m->sam, addr - 0xFFC0);
            // Update VDG display base address from SAM F0-F6
            m->vdg.vram_offset = sam6883_get_vdg_address(&m->sam);
            // SAM V0-V2 control VDG GM0-GM2 bits
            update_vdg_mode(m);
            return;
        }

        // $FFE0-$FFFF: ROM vectors, writes ignored
        return;
    }

    // --- ROM space: $8000-$FEFF ---
    // SAM MAP TYPE (ty) = 1: all-RAM mode — writes go to RAM
    if (addr >= 0x8000) {
        if (m->sam.ty && (size_t)addr < m->ram_size) {
            m->ram[addr] = val;
        }
        // Normal mode: writes to ROM space are ignored
        return;
    }

    // --- RAM: $0000-$7FFF ---
    if ((size_t)addr < m->ram_size) {
        m->ram[addr] = val;
    }
}

// ============================================================
// Memory allocation helper
// ============================================================

static uint8_t* machine_alloc(size_t size, const char* label) {
    uint8_t* ptr = nullptr;

#if USE_PSRAM
    if (ESP.getPsramSize() > 0) {
        ptr = (uint8_t*)ps_malloc(size);
        if (ptr) {
            DEBUG_PRINTF("  %s: %d bytes from PSRAM", label, (int)size);
            return ptr;
        }
        DEBUG_PRINTF("  %s: PSRAM alloc failed, falling back to heap", label);
    }
#endif

    ptr = (uint8_t*)malloc(size);
    if (ptr) {
        DEBUG_PRINTF("  %s: %d bytes from heap", label, (int)size);
    } else {
        Serial.printf("FATAL: %s allocation failed (%d bytes)!\n", label, (int)size);
    }
    return ptr;
}

// ============================================================
// Public API: init
// ============================================================

void machine_init(Machine* m) {
    DEBUG_PRINT("Machine: initializing...");
    memset(m, 0, sizeof(Machine));
    g_machine = m;

    // --- Allocate memory ---
    m->ram_size = COCO_RAM_SIZE;
    m->ram = machine_alloc(m->ram_size, "RAM");
    if (!m->ram) return;

    m->rom_basic = machine_alloc(8192, "ROM-BASIC");
    if (!m->rom_basic) return;

    m->rom_extbas = machine_alloc(8192, "ROM-ExtBAS");
    if (!m->rom_extbas) return;

    m->rom_cart = machine_alloc(16384, "ROM-Cart");
    if (!m->rom_cart) return;

    // Fill ROM buffers with $FF (unloaded state)
    memset(m->rom_basic, 0xFF, 8192);
    memset(m->rom_extbas, 0xFF, 8192);
    memset(m->rom_cart, 0xFF, 16384);

    // --- Initialize core chips ---
    mc6809_init(&m->cpu);
    m->cpu.read = machine_read;
    m->cpu.write = machine_write;

    mc6821_init(&m->pia0);
    mc6821_init(&m->pia1);

    // Wire IRQ callbacks
    m->pia0.irq_a_callback = pia0_irq_a_callback;
    m->pia0.irq_b_callback = pia0_irq_b_callback;
    m->pia1.irq_a_callback = pia1_irq_a_callback;
    m->pia1.irq_b_callback = pia1_irq_b_callback;

    mc6847_init(&m->vdg);
    sam6883_init(&m->sam);

    // --- Timing ---
    m->ntsc = true;
    m->cycles_per_frame = CYCLES_PER_FRAME;
    m->cart_inserted = false;

    m->initialized = true;
    DEBUG_PRINTF("Machine: init complete. Free heap: %d", ESP.getFreeHeap());
}

// ============================================================
// Public API: load ROMs
// ============================================================

bool machine_load_roms(Machine* m, const char* rom_path) {
    if (!m->initialized) return false;

    DEBUG_PRINT("Machine: loading ROMs...");
    char path[64];

    // Color BASIC ROM → $A000-$BFFF (8K)
    snprintf(path, sizeof(path), "%s/%s", rom_path, ROM_BASIC_FILE);
    if (hal_storage_load_file(path, m->rom_basic, 8192)) {
        m->rom_basic_loaded = true;
        DEBUG_PRINTF("  Loaded %s → $A000-$BFFF", ROM_BASIC_FILE);
    } else {
        DEBUG_PRINTF("  MISSING: %s", path);
    }

    // Extended BASIC ROM → $8000-$9FFF (8K)
    snprintf(path, sizeof(path), "%s/%s", rom_path, ROM_EXT_BASIC_FILE);
    if (hal_storage_load_file(path, m->rom_extbas, 8192)) {
        m->rom_extbas_loaded = true;
        DEBUG_PRINTF("  Loaded %s → $8000-$9FFF", ROM_EXT_BASIC_FILE);
    } else {
        DEBUG_PRINTF("  MISSING: %s", path);
    }

    // Disk BASIC / Cartridge ROM → $C000-$DFFF (8K-16K, optional)
    snprintf(path, sizeof(path), "%s/%s", rom_path, ROM_DISK_FILE);
    if (hal_storage_load_file(path, m->rom_cart, 8192)) {
        m->rom_cart_loaded = true;
        m->cart_inserted = true;
        DEBUG_PRINTF("  Loaded %s → $C000", ROM_DISK_FILE);
    } else {
        DEBUG_PRINTF("  Optional: %s not found", path);
    }

    // Verify reset vector is readable
    if (m->rom_basic_loaded) {
        uint16_t reset_vec = ((uint16_t)m->rom_basic[0x1FFE] << 8) | m->rom_basic[0x1FFF];
        DEBUG_PRINTF("  Reset vector in BASIC ROM: $%04X", reset_vec);
    }

    return m->rom_basic_loaded;
}

// ============================================================
// Public API: reset
// ============================================================

void machine_reset(Machine* m) {
    if (!m->initialized) return;

    DEBUG_PRINT("Machine: RESET");

    // Clear RAM
    memset(m->ram, 0x00, m->ram_size);

    // Reset all components
    mc6821_reset(&m->pia0);
    mc6821_reset(&m->pia1);
    mc6847_reset(&m->vdg);
    sam6883_reset(&m->sam);

    // Re-wire IRQ callbacks (reset clears them)
    m->pia0.irq_a_callback = pia0_irq_a_callback;
    m->pia0.irq_b_callback = pia0_irq_b_callback;
    m->pia1.irq_a_callback = pia1_irq_a_callback;
    m->pia1.irq_b_callback = pia1_irq_b_callback;

    // Set VDG VRAM pointer to machine RAM
    m->vdg.vram = m->ram;
    m->vdg.vram_offset = sam6883_get_vdg_address(&m->sam);
    m->vdg.row_address = m->vdg.vram_offset;
    // Reset SAM VDG counter
    sam6883_vdg_fsync(&m->sam, true);

    // Reset timing state
    m->cycles_this_frame = 0;
    m->scanline = 0;
    m->frame_count = 0;

    // CPU reset last (reads reset vector from ROM)
    mc6809_reset(&m->cpu);
}

// Precomputed per-scanline cycle targets (avoids multiply+divide per scanline)
static uint16_t scanline_cycle_targets[SCANLINES_PER_FRAME];

// ============================================================
// Public API: run one scanline
// ============================================================

void machine_run_scanline(Machine* m) {
    if (!m->initialized) return;

    // Use precomputed table (filled in machine_run_frame) to avoid
    // per-scanline multiply+divide
    int cycles_to_run = (int)(scanline_cycle_targets[m->scanline] - m->cycles_this_frame);
    if (cycles_to_run < 1) cycles_to_run = 1;

    // Tick FDC deferred INTRQ (fires NMI after sector transfer completes)
    sv_disk_tick(&m->fdc);

    // Execute CPU
    int actual = mc6809_run(&m->cpu, cycles_to_run);
    m->cycles_this_frame += actual;

    // --- VDG/SAM scanline processing ---

    // FS (field sync) — PIA0 CB1 — 60Hz interrupt
    if (m->scanline == 0) {
        // End of vertical blank: FS goes HIGH (rising edge)
        m->vdg.fs = false;
        mc6821_cb1_transition(&m->pia0, true);
        // Reset SAM VDG address counter to display base
        sam6883_vdg_fsync(&m->sam, true);
    }

    if (m->scanline < ACTIVE_SCANLINES) {
        // Get row address BEFORE fetch advancement (VDG reads from current address)
        m->vdg.row_address = sam6883_get_vdg_row_address(&m->sam);
        m->vdg.scanline = m->scanline;
        if (mc6847_render_scanline(&m->vdg)) {
            hal_video_render_scanline(m->scanline, m->vdg.line_buffer, VDG_ACTIVE_WIDTH);
        }

        // Simulate VDG data fetch: advance SAM counter by bytes_per_row.
        // In XRoar, sam_vdg_bytes() is called by VDG as it reads each byte.
        // We do it in one call after rendering with the total for the row.
        // bytes_per_row: GM=0,1,3,5→16, GM=2,4,6,7→32
        // Text mode also fetches 32 bytes (handled by SAM divide counters).
        {
            static const int bytes_per_row[8] = { 16, 16, 32, 16, 32, 16, 32, 32 };
            int bpr = (m->vdg.mode & VDG_AG) ? bytes_per_row[m->sam.vdg_mode] : 32;
            sam6883_vdg_fetch_bytes(&m->sam, bpr);
        }
    }

    // HS (horizontal sync) — supplementary counter adjustment per XRoar.
    // On real CoCo: VDG HS → PIA0 CA1, but CoCo BASIC doesn't enable
    // CA1 IRQ so we skip PIA transitions for performance.
    sam6883_vdg_hsync(&m->sam, false);  // Falling edge does fixup

    if (m->scanline == ACTIVE_SCANLINES) {
        // Start of vertical blank: FS goes LOW (falling edge)
        m->vdg.fs = true;
        mc6821_cb1_transition(&m->pia0, false);
    }

    m->scanline++;
}

// ============================================================
// Public API: run one frame (262 scanlines)
// ============================================================

void machine_run_frame(Machine* m) {
    if (!m->initialized) return;

    // Precompute cycle targets for all 262 scanlines
    for (int i = 0; i < SCANLINES_PER_FRAME; i++) {
        scanline_cycle_targets[i] = ((i + 1) * m->cycles_per_frame) / SCANLINES_PER_FRAME;
    }

    m->cycles_this_frame = 0;
    m->scanline = 0;

    for (int line = 0; line < SCANLINES_PER_FRAME; line++) {
        machine_run_scanline(m);
        // Capture audio level after each scanline for pitch-correct playback
        hal_audio_capture_scanline();
    }

    // Commit scanline audio buffer to ISR for playback at correct CoCo rate
    hal_audio_commit_frame();

    // Present completed frame (with VRAM shadow compare — OPT-16)
    hal_video_present(m->ram, m->sam.vdg_base, m->vdg.mode);

    m->frame_count++;
}

// ============================================================
// Public API: free
// ============================================================

void machine_free(Machine* m) {
    if (m->ram)        { free(m->ram);        m->ram = nullptr; }
    if (m->rom_basic)  { free(m->rom_basic);  m->rom_basic = nullptr; }
    if (m->rom_extbas) { free(m->rom_extbas); m->rom_extbas = nullptr; }
    if (m->rom_cart)   { free(m->rom_cart);    m->rom_cart = nullptr; }
    m->initialized = false;
    DEBUG_PRINT("Machine: freed");
}
