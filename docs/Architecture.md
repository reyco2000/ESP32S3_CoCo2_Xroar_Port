
# ESP32_CoCo2_XRoar_Port Architecture Document

## Project Overview

ESP32_CoCo2_XRoar_Port is a Tandy Color Computer 2 (CoCo 2) emulator running on an ESP32-S3 microcontroller. It is derived from XRoar, Ciaran Anscomb's Dragon
/CoCo emulator originally written in C for desktop platforms. The project adapts XRoar's emulation core to run on a resource-constrained embedded
system with custom hardware abstraction for display, audio, keyboard, and storage.

**Target hardware:** ESP32-S3 N16R8 (dual-core 240 MHz, 16 MB flash, 8 MB PSRAM)

**Emulated machine:** Tandy CoCo 2 with 64 KB RAM, Color BASIC 1.3, Extended BASIC 1.1, Disk BASIC 1.1

---

## System Architecture

```
+-----------------------------------------------------------+
|               ESP32_CoCo2_XRoar_Port.ino                    |
|                   (Arduino entry point)                    |
|  setup() -> hal_init, machine_init, load_roms, video_init |
|  loop()  -> hal_process_input, supervisor, machine_run    |
+-----------------------------------------------------------+
         |                    |                    |
         v                    v                    v
+----------------+  +------------------+  +----------------+
|  HAL Layer     |  |  Emulation Core  |  |  Supervisor    |
|  (hal/*.cpp)   |  |  (core/*.cpp)    |  |  (supervisor/) |
|                |  |                  |  |                |
| hal_video      |  | MC6809 CPU       |  | OSD Menu       |
| hal_audio      |  | MC6821 PIA x2    |  | Disk Manager   |
| hal_keyboard   |  | MC6847 VDG       |  | File Browser   |
| hal_joystick   |  | SAM6883          |  | FDC (WD1793)   |
| hal_storage    |  | Machine wiring   |  | Render engine  |
| usb_kbd_host   |  |                  |  |                |
+----------------+  +------------------+  +----------------+
         |                    |                    |
         v                    v                    v
+-----------------------------------------------------------+
|              ESP32-S3 Hardware                             |
|  TFT SPI | LEDC PWM | USB Host | SD SPI | GPIO | PSRAM   |
+-----------------------------------------------------------+
```

---

## Module Breakdown

### 1. Entry Point: `ESP32_CoCo2_XRoar_Port.ino`

The Arduino sketch orchestrates boot and the main loop.

**Boot sequence (setup):**
1. Initialize Serial (115200 baud)
2. `hal_init()` — storage, audio, keyboard, joystick (NOT video yet)
3. `machine_init()` — allocate RAM/ROM from PSRAM, init CPU/PIA/VDG/SAM
4. `machine_load_roms()` — load bas13.rom, extbas11.rom, disk11.rom from SD card
5. `hal_video_init()` — initialize TFT display (after SD, since they share SPI)
6. `machine_reset()` — cold reset, read reset vector
7. `supervisor_init()` — OSD menu, FDC, NMI wiring
8. `supervisor_load_state()` — restore last-mounted disks from NVS
9. Wait up to 3s for USB keyboard enumeration

**Main loop (loop):**
1. `hal_process_input()` — drain USB keyboard queue, tick deferred releases
2. `supervisor_update_and_render()` — if active, render OSD and skip emulation
3. `machine_run_frame()` — execute 262 scanlines of CPU + VDG
4. `hal_render_frame()` — push sprite to TFT

---

### 2. Emulation Core (`src/core/`)

#### MC6809 CPU — `mc6809.cpp` / `mc6809.h`

Full Motorola 6809 CPU emulation with all documented and undocumented opcodes.

- **Registers:** A, B (combined as D), X, Y, U (user stack), S (hardware stack), DP (direct page), CC (condition codes), PC
- **Addressing modes:** Inherent, Immediate, Direct, Extended, Indexed (all 6809 post-byte variants)
- **Interrupts:** NMI, FIRQ, IRQ with proper priority and masking
- **Special states:** CWAI (push-and-wait), SYNC, HALT
- **Cycle accuracy:** Each instruction accounts for correct cycle counts
- **Memory interface:** Function pointers `read(addr)` and `write(addr, val)` set by machine.cpp
- **Debug:** PC trace ring buffer (64 entries), F4 hotkey dumps trace to serial

Key implementation details:
- `mc6809_run(cpu, budget)` executes instructions until cycle budget exhausted
- `check_interrupts()` called before each instruction — dispatches NMI/FIRQ/IRQ
- `nmi_armed` flag prevents NMI before first LDS instruction (per 6809 spec)
- CWAI pre-pushes state, then waits; interrupt handler skips redundant push

#### MC6821 PIA — `mc6821.cpp` / `mc6821.h`

Two Peripheral Interface Adapters handle all I/O and interrupts.

**PIA0 ($FF00-$FF03):** Keyboard matrix, joystick comparator, vsync IRQ
- Port A (PA0-PA7): Keyboard row data input (active low)
- Port B (PB0-PB7): Keyboard column select output
- CA1: Horizontal sync (HS) from VDG
- CB1: Field sync (FS/vsync) from VDG — triggers 60 Hz IRQ
- IRQA/IRQB -> CPU IRQ line

**PIA1 ($FF20-$FF23):** Sound output, VDG mode control, cartridge interrupt
- Port A bits 2-7: 6-bit DAC audio output
- Port B bit 1: Single-bit audio output
- Port B bits 3-4: VDG mode (CSS, GM selects)
- CA1/CB1: Cartridge FIRQ
- IRQA/IRQB -> CPU FIRQ line

#### MC6847 VDG — `mc6847.cpp` / `mc6847.h`

Video Display Generator renders 256x192 active pixels into a line buffer.

**Text mode (AG=0):**
- 32x16 characters, 8px wide x 12 scanlines tall
- Internal 6x12 font ROM (64 characters, PROGMEM)
- Supports normal, inverse, and semigraphics-4 modes
- CoCo BASIC stores visible text as VDG "inverse" ($40-$7F)

**Graphics modes (AG=1, GM=0-7):**
- 8 modes from CG1 (64x64, 4-color) to RG6 (256x192, 2-color)
- 1bpp modes: 2 colors per CSS (green/dark-green or orange/dark-orange)
- 2bpp modes: 4-color palettes (green/yellow/blue/red or white/cyan/magenta/orange)
- Row address provided by SAM counter; VDG reads from VRAM via `row_address`

**Color palette:** 12 VDG colors mapped to RGB565 in hal_video.cpp

#### SAM6883 — `sam6883.cpp` / `sam6883.h`

Synchronous Address Multiplexer controls memory mapping and VDG display address.

**Register ($FFC0-$FFDF):** 16-bit register, accessed as 32 bit set/clear pairs
- V0-V2: VDG mode (graphics resolution)
- F0-F6: Display base address (64-byte granularity)
- M0-M1: Memory size (4K/16K/64K)
- TY: Map type (ROM/all-RAM)

**VDG address counter:**
- Resets to `vdg_base` on field sync (vsync)
- Advances per scanline via two mechanisms:
  1. `sam6883_vdg_fetch_bytes(nbytes)` — simulates VDG data clock (XRoar's `sam_vdg_bytes`)
  2. `sam6883_vdg_hsync()` — supplementary fixup at end of scanline
- Divide-by-X/Y counters implement row repetition for lower-resolution modes

**Critical tables (matching XRoar):**
```
GM:        0    1    2    3    4    5    6    7
bytes/row: 16   16   32   16   32   16   32   32
add(hsync):16   8    16   8    16   8    16   0
xdiv:      1    3    1    2    1    1    1    1
ydiv:      12   1    3    1    2    1    1    1
```

#### Machine Integration — `machine.cpp` / `machine.h`

Wires all components together with CoCo 2 memory map.

**Memory map dispatch (machine_read / machine_write):**
```
$0000-$7FFF  RAM (64 KB, SAM page select)
$8000-$9FFF  Extended BASIC ROM (8K)
$A000-$BFFF  Color BASIC ROM (8K)
$C000-$FEFF  Disk BASIC ROM (16K cartridge slot)
$FF00-$FF1F  PIA0 (mirrored every 4 bytes)
$FF20-$FF3F  PIA1 (mirrored every 4 bytes)
$FF40-$FF5F  FDC — WD1793 + DSKREG
$FFC0-$FFDF  SAM control register
$FFE0-$FFFF  Vectors (from ROM)
```

**IRQ routing:**
- PIA0 IRQA/IRQB -> CPU IRQ (60 Hz timer, keyboard)
- PIA1 IRQA/IRQB -> CPU FIRQ (cartridge)

**Frame execution (`machine_run_frame`):**
- 262 scanlines per NTSC frame
- Each scanline: tick FDC, run CPU for ~57 cycles, render VDG, advance SAM counter
- Field sync on scanline 0 (rising) and scanline 192 (falling)
- SAM fetch + hsync on every active scanline

---

### 3. Hardware Abstraction Layer (`src/hal/`)

#### Video — `hal_video.cpp`

- TFT_eSPI library driving ILI9341/ST7789 320x240 SPI display
- 256x192 sprite (TFT_eSprite) in PSRAM, centered at offset (32, 24) on TFT
- Palette: 12 VDG colors -> RGB565 lookup table
- Frame skipping: pushes sprite every 2nd frame to reduce SPI blocking
- Exposes `hal_video_get_tft()` for supervisor direct TFT access

#### Audio — `hal_audio.cpp`

- LEDC PWM on GPIO17 at 22050 Hz ISR
- Receives 6-bit DAC values (PIA1 port A bits 2-7) and single-bit audio (PIA1 port B bit 1)
- Timer ISR reads current DAC+bit values and writes PWM duty cycle

#### Keyboard — `hal_keyboard.cpp` + `usb_kbd_host.cpp`

**USB Host (Core 0):**
- ESP-IDF USB Host library + HID class driver
- Runs on Core 0 via FreeRTOS tasks
- Boot protocol keyboard: 8-byte reports
- Key events sent to Core 1 via FreeRTOS queue (32 entries)

**Matrix Injection (Core 1):**
- `hid_host_process()` drains queue, calls `on_hid_key()` callback
- HID usage codes mapped to CoCo keyboard matrix positions (8 columns x 7 rows)
- Deferred release mechanism: keys held for minimum 4 frames to prevent same-frame press+release
- Special keys: ESC=BREAK, Backspace=CLEAR, F1=supervisor, F2=reset, F3=quick mount, F4=trace dump, F5=FPS

**CoCo keyboard matrix:**
```
        PB0  PB1  PB2  PB3  PB4  PB5  PB6  PB7
PA0:     @    A    B    C    D    E    F    G
PA1:     H    I    J    K    L    M    N    O
PA2:     P    Q    R    S    T    U    V    W
PA3:     X    Y    Z   UP  DOWN LEFT RIGHT SPACE
PA4:     0    1    2    3    4    5    6    7
PA5:     8    9    :    ;    ,    -    .    /
PA6:   ENTER CLEAR BREAK ---  ---  ---  ---  SHIFT
```

#### Storage — `hal_storage.cpp`

- SD card on dedicated SPI bus (not shared with TFT): SCLK=39, MOSI=40, MISO=41, CS=38
- Initialized at 4 MHz
- Provides `hal_storage_load_file()` for ROM loading
- SD `File` objects kept open for mounted disk images

#### Joystick — `hal_joystick.cpp`

- Stub implementation (TODO)
- Pin assignments: JOY0 X=9, Y=8, BTN=18; JOY1 X=16, Y=15, BTN=7

---

### 4. Supervisor / OSD (`src/supervisor/`)

Provides an on-screen display menu system for disk management and settings.

#### State Machine — `supervisor.cpp` / `supervisor.h`

States: `INACTIVE -> MAIN_MENU -> FILE_BROWSER | DISK_MANAGER | ABOUT | CONFIRM_DIALOG`

- F1 toggles supervisor on/off
- While active: emulation paused, supervisor renders directly to TFT
- On deactivate: fills OSD area with black, emulator repaints on next frame

#### Menu System — `sv_menu.cpp` / `sv_menu.h`

Main menu items: Resume, Disk Manager, Machine type, Reset, About
- Arrow keys navigate, Enter selects
- ESC/F1 closes supervisor

#### File Browser — `sv_filebrowser.cpp` / `sv_filebrowser.h`

- Scans SD card directories
- Shows directories (yellow "+"), supported .DSK/.VDK files (cyan "*"), unsupported (red "!")
- Enter on a .DSK file mounts it to the selected drive
- Scrollbar for long listings

#### Disk Controller — `sv_disk.cpp` / `sv_disk.h`

WD1793-compatible floppy disk controller with INTRQ tracking.

**Register map:**
- $FF40-$FF47: DSKREG (drive select latch) — bits 0-2=drive, 3=motor, 6=HALT enable, 7=NMI enable
- $FF48: Command/Status register
- $FF49: Track register
- $FF4A: Sector register
- $FF4B: Data register

**Command-level emulation (no timing):**
- Type I: RESTORE ($00), SEEK ($10), STEP ($20-$70)
- Type II: READ SECTOR ($80/$90), WRITE SECTOR ($A0/$B0)
- Type IV: FORCE INTERRUPT ($D0)

**INTRQ/NMI mechanism (critical for DSKCON compatibility):**
1. DSKCON issues FDC command
2. Command completes instantly, sets `intrq = true`
3. DSKCON writes DSKREG to enable NMI (bit 7)
4. `try_fire_nmi()` checks both `intrq` AND `nmi_enabled` -> fires `mc6809_nmi()`
5. INTRQ cleared on status register read or new command write

**Deferred INTRQ for sector transfers:**
- After 256th byte read/write, `intrq_defer = 1` (not immediate)
- `sv_disk_tick()` called once per scanline, decrements counter
- When counter reaches 0, fires `set_intrq()`
- This gives the CPU time to execute STA ,X+ (store last byte) before NMI fires
- On real hardware, WD1793 reads CRC bytes (~64 us) before INTRQ

**Disk image support:**
- .DSK (JVC format): header = file_size % 256, then raw sector data
- .VDK format: 12-byte header
- Geometry: typically 35 tracks, 18 sectors/track, 256 bytes/sector
- Supports up to 4 drives, read-write, with SD file handles kept open

#### Render Engine — `sv_render.cpp` / `sv_render.h`

- Green phosphor aesthetic (dark green bg, bright green text)
- Window: 256x192 at offset (32,24) — matches emulator sprite area exactly
- Font 2 (16px), 18px item height, 8 visible menu items
- Direct TFT rendering via TFT_eSPI (not through sprite)

---

### 5. Configuration (`config.h`)

Compile-time settings for the entire project:

| Setting | Value | Description |
|---------|-------|-------------|
| MACHINE_TYPE | 3 | CoCo 2 |
| CPU_VARIANT | 0 | MC6809 |
| RAM_SIZE_KB | 64 | 64 KB main RAM |
| CPU_CLOCK_HZ | 895000 | 0.895 MHz NTSC |
| TARGET_FPS | 60 | NTSC timing |
| CYCLES_PER_FRAME | 14916 | 895000/60 |
| SCANLINES_PER_FRAME | 262 | NTSC |
| ACTIVE_SCANLINES | 192 | Visible display lines |
| DISPLAY_WIDTH x HEIGHT | 320x240 | TFT resolution |
| AUDIO_SAMPLE_RATE | 22050 | PWM audio rate |
| USE_PSRAM | 1 | RAM/ROM in PSRAM |

---

### 6. Pin Assignments

| Function | GPIO | Notes |
|----------|------|-------|
| TFT CS | 10 | SPI display |
| TFT DC | 2 | Data/Command |
| TFT RST | 4 | Reset |
| TFT MOSI | 11 | SPI data |
| TFT SCLK | 12 | SPI clock |
| TFT BL | 5 | Backlight |
| SD CS | 38 | Dedicated SPI bus |
| SD MOSI | 40 | |
| SD MISO | 41 | |
| SD SCLK | 39 | |
| Audio PWM | 17 | LEDC output |
| USB D- | 19 | USB Host keyboard |
| USB D+ | 20 | USB Host keyboard |
| Joy0 X/Y/BTN | 9/8/18 | Analog joystick 0 |
| Joy1 X/Y/BTN | 16/15/7 | Analog joystick 1 |

---

### 7. Build System

Arduino CLI with ESP32 board support:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=16M,PSRAM=opi ESP32_CoCo2_XRoar_Port/
arduino-cli upload --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=16M,PSRAM=opi --port /dev/ttyACM0 ESP32_CoCo2_XRoar_Port/
```

**Critical build flags:**
- `USBMode=default` — USB OTG in host mode for keyboard
- `CDCOnBoot=default` — Serial monitor via UART (not USB CDC)
- `PSRAM=opi` — Enable 8 MB octal PSRAM

**Dependencies:**
- TFT_eSPI (display driver, configured via User_Setup.h)
- ESP32_USB_Host_HID (USB keyboard support)
- SD library (SD card access)

---

### 8. ROM Requirements

Proprietary ROM images required (not included in source):

| File | Size | Maps to | Description |
|------|------|---------|-------------|
| bas13.rom | 8 KB | $A000-$BFFF | Color BASIC 1.3 |
| extbas11.rom | 8 KB | $8000-$9FFF | Extended BASIC 1.1 |
| disk11.rom | 8 KB | $C000-$DFFF | Disk BASIC 1.1 |

Place in `/roms/` directory on SD card.

---

### 9. Performance Characteristics

- CPU emulation: ~23.5 fps (0.4x realtime)
- Frame skip: every 2nd frame pushed to TFT
- Display: ~40ms SPI transfer per frame push
- Audio: Real-time via LEDC timer ISR
- Memory: ~330 KB free heap after init, 64 KB RAM + 40 KB ROM in PSRAM

---

### 10. Data Flow Diagrams

#### Frame Execution Flow
```
machine_run_frame()
  for scanline = 0..261:
    sv_disk_tick()              // Deferred INTRQ countdown
    mc6809_run(~57 cycles)      // CPU executes instructions
      check_interrupts()        //   NMI > FIRQ > IRQ dispatch
      execute_one()             //   Fetch-decode-execute
        machine_read/write()    //   Memory/IO dispatch
    if scanline < 192:
      row_addr = SAM counter    // Get VRAM row address
      mc6847_render_scanline()  // VDG renders to line_buffer
      hal_video_render_scanline() // Copy to sprite
      sam_vdg_fetch_bytes()     // Advance SAM counter (data fetch)
    sam_vdg_hsync()             // SAM counter fixup
    if scanline == 0: FS rising   // vsync start
    if scanline == 192: FS falling // vsync end -> 60Hz IRQ
  hal_video_present()           // Push sprite to TFT (every 2nd frame)
```

#### Keyboard Input Flow
```
USB Keyboard (physical)
  -> ESP-IDF USB Host (Core 0 task)
  -> HID boot protocol report (8 bytes)
  -> process_keyboard_report() diffs prev/current
  -> FreeRTOS queue (key_event_t)
  -> hid_host_process() on Core 1 (called from hal_process_input)
  -> on_hid_key() callback
  -> Maps HID usage -> CoCo matrix (row, col)
  -> set_key(col, row, pressed) modifies key_matrix[]
  -> Deferred release queue (4-frame minimum hold)
  -> PIA0 reads key_matrix[col] when CPU scans keyboard
```

#### Disk Read Flow (DSKCON)
```
BASIC: LOAD"FILE"
  -> Disk BASIC ROM reads directory (T17, S3-11)
  -> For each sector:
    1. CPU writes track/sector to FDC registers ($FF49/$FF4A)
    2. CPU writes READ SECTOR command to $FF48
       -> fdc_execute_command(): reads 256 bytes from SD into sector_buf
       -> Sets reading=true, drq=true, data=sector_buf[0]
    3. CPU writes DSKREG ($FF40) to enable HALT + NMI
    4. CPU enters tight loop: LDA $FF4B / STA ,X+ / BRA loop
       -> Each LDA $FF4B calls fdc_read_data()
       -> Returns current byte, advances buf_pos, pre-loads next
    5. After 256th byte: sets intrq_defer=1 (not immediate NMI)
    6. CPU executes STA ,X+ (stores last byte)
    7. Next scanline: sv_disk_tick() fires set_intrq() -> NMI
    8. NMI handler returns to DSKCON, which sets up next sector
```

---

### 11. Source File Reference

```
ESP32_CoCo2_XRoar_Port/
  ESP32_CoCo2_XRoar_Port.ino  Main sketch (setup + loop)
  config.h                 Hardware config, pin assignments, timing

  src/core/
    machine.cpp/.h         System integration, memory map, frame loop
    mc6809.cpp/.h          MC6809 CPU emulation (2700+ lines)
    mc6809_opcodes.h       Opcode tables
    mc6821.cpp/.h          MC6821 PIA (2 instances)
    mc6847.cpp/.h          MC6847 VDG (text + 8 graphics modes)
    sam6883.cpp/.h         SAM6883 (address mux + VDG counter)

  src/hal/
    hal.h                  HAL interface (all subsystem declarations)
    hal.cpp                Top-level hal_init, hal_process_input
    hal_video.cpp          TFT_eSPI display + sprite rendering
    hal_audio.cpp          LEDC PWM audio output
    hal_keyboard.cpp       CoCo matrix + HID mapping + deferred release
    hal_joystick.cpp       Joystick stub (TODO)
    hal_storage.cpp        SD card init + file loading
    usb_kbd_host.cpp/.h    ESP-IDF USB Host HID driver (Core 0)

  src/supervisor/
    supervisor.cpp/.h      OSD lifecycle, state machine, toggle
    sv_menu.cpp/.h         Main menu rendering + key handling
    sv_filebrowser.cpp/.h  SD card directory browser
    sv_disk.cpp/.h         WD1793 FDC emulation + INTRQ/NMI
    sv_render.cpp/.h       OSD rendering engine (green phosphor theme)

  src/tests/
    integration_test.cpp/.h  Automated test framework (5 tests)

  src/utils/
    debug.h                DEBUG_PRINT/DEBUG_PRINTF macros

  src/roms/
    rom_loader.cpp/.h      ROM file loading utilities
```
