# Supervisor Module — CoCo_ESP32 OSD

## Overview

The Supervisor is an **On-Screen Display (OSD) overlay** that pauses emulation and provides disk management, machine reset, and settings via a menu UI rendered directly to the ILI9341 TFT. It uses a **blue theme** (bright blue-white on dark blue).

**Activation:** F1 toggles the overlay on/off. While active, `supervisor_update_and_render()` returns `true`, telling the main loop to skip emulation.

---

## File Map

| File | Purpose |
|---|---|
| `supervisor.h/.cpp` | Core lifecycle, state machine, public API, state persistence |
| `sv_menu.h/.cpp` | Main menu definition, actions, and key handling |
| `sv_filebrowser.h/.cpp` | SD card directory browser for mounting .DSK/.VDK images |
| `sv_disk.h/.cpp` | WD1793 FDC emulation, PSRAM disk caching, mount/eject/flush |
| `sv_render.h/.cpp` | TFT rendering primitives (frame, menu items, file entries, dialogs) |
| `sv_debug.h/.cpp` | Debug memory dump to serial (Motorola S-Record / Intel HEX) |

---

## State Machine

```
SV_INACTIVE ──F1──> SV_MAIN_MENU
                       │
          ┌────────────┼────────────────┬──────────┐
          │            │                │          │
    SV_FILE_BROWSER  SV_DISK_MANAGER  SV_ABOUT  SV_DEBUG_DUMP
          │            │
          │     SV_FILE_BROWSER (from disk mgr)
          │
    SV_CONFIRM_DIALOG (e.g. reset confirmation)
```

States are defined in `supervisor.h` as `SV_State` enum:

| State | Description |
|---|---|
| `SV_INACTIVE` | OSD hidden, emulation running |
| `SV_MAIN_MENU` | Top-level menu with 6 items |
| `SV_FILE_BROWSER` | SD card browser for disk image selection |
| `SV_DISK_MANAGER` | Shows drives 0-3, mount/eject controls |
| `SV_ABOUT` | Info screen (version, free heap) |
| `SV_DEBUG_DUMP` | Debug memory dump to serial port |
| `SV_CONFIRM_DIALOG` | Yes/No dialog (used by Reset) |
| `SV_MACHINE_SELECT` | Placeholder (TODO) |
| `SV_SETTINGS` | Placeholder (TODO) |

**Transitions** are driven by `sv.state` assignment. `sv.prev_state` tracks where to return on ESC/cancel.

---

## Main Loop Integration

In the main loop (typically `loop()` in the .ino):

```
if (supervisor_update_and_render()) {
    return;  // skip emulation this frame
}
// ... run emulation ...
```

Key dispatch from USB keyboard:

```
if (supervisor_is_active()) {
    supervisor_on_key(hid_usage, pressed);
} else if (hid_usage == 0x3A && pressed) {  // F1
    supervisor_toggle();
}
```

---

## Data Flow

### Supervisor_t Struct (supervisor.h)

Single global instance (`static Supervisor_t sv`). Contains:

- **state/prev_state** — current screen and return target
- **machine** — pointer to the `Machine` struct (CPU, PIA, FDC, RAM)
- **menu_cursor / menu_scroll_offset / menu_item_count** — shared cursor state reused by main menu and disk manager
- **current_path / file_cursor / file_scroll_offset / file_count / target_drive** — file browser state
- **file_entries** — heap-allocated array of `SV_FileEntry[128]` (allocated once on first browse)
- **confirm_message / confirm_callback / confirm_context** — generic confirmation dialog
- **needs_redraw** — dirty flag; rendering only happens when true

### Redraw Model

The OSD uses a **dirty-flag** redraw approach:
1. Key handlers set `sv.needs_redraw = true` on any visual change
2. `supervisor_update_and_render()` checks the flag, renders once, clears it
3. When no redraw is needed, a 16ms delay caps at ~60fps idle

---

## Module Details

### sv_menu.cpp — Main Menu

**Menu items** (static array `menu_items[]`):

| Index | Label | Action |
|---|---|---|
| 0 | Browse/Mount Disk | Opens file browser for drive 0 |
| 1 | Select Drive - Eject Disk | Opens drive status screen |
| 2 | Machine: CoCo 2 | TODO: machine selection |
| 3 | Reset Machine | Confirm dialog -> `machine_reset()` |
| 4 | Debug Dump | Memory dump to serial (S-Record / Intel HEX) |
| 5 | About | Info screen |

**Key handling:** Up/Down move cursor, Enter executes action, ESC/F1 close.

`execute_action()` is the dispatcher — modifies `sv.state` to transition screens. Reset uses the confirm dialog pattern with a lambda callback.

### sv_filebrowser.cpp — File Browser

Browses the SD card FAT32 filesystem. Key behaviors:

- **scan:** `sv_fb_scan_directory()` reads directory entries into `SV_FileEntry[]` (max 128)
- **sort:** Directories first (`.."` always first), then supported files (.dsk/.vdk), then others, alphabetical within groups
- **navigate:** Enter on dir = descend, Backspace = parent, PgUp/PgDn/Home/End for fast scrolling
- **select:** Enter on a supported file = mount to `target_drive` and close OSD
- **SPI bus note:** SD reads happen BEFORE `tft.startWrite()` — they share the SPI bus

**Supported formats:** `.dsk` (JVC) and `.vdk` = fully supported. `.dmk` = recognized but not mountable.

Visible window is 9 items (`SV_FB_VISIBLE_ITEMS`). Scrollbar rendered when list exceeds window.

### sv_disk.cpp — FDC Emulation

Full WD1793 command-level emulation mapped at `$FF40-$FF5F`:

**Address map:**
| Range | Read | Write |
|---|---|---|
| `$FF40-$FF47` | Returns 0 | DSKREG (drive select latch) |
| `$FF48` | Status | Command |
| `$FF49` | Track | Track |
| `$FF4A` | Sector | Sector |
| `$FF4B` | Data (read sector) | Data (write sector) |

**DSKREG ($FF40) bits** — matches XRoar `rsdos.c`:
- Bits 0-2: drive select (one-hot: 0x01=D0, 0x02=D1, 0x04=D2)
- Bit 3: motor on
- Bit 5: density (XOR'd with 0x20 — inverted so normal CoCo setting enables NMI)
- Bit 7: HALT enable (DRQ -> CPU HALT for byte synchronization)

**Commands implemented:**
| Cmd | Type | Notes |
|---|---|---|
| `$00` | Restore | Track=0, check mounted |
| `$10` | Seek | Track=Data register |
| `$20-$7F` | Step/StepIn/StepOut | Updates track if bit 4 set |
| `$80/$90` | Read Sector | Copies from PSRAM cache to sector_buf |
| `$A0/$B0` | Write Sector | Fills sector_buf, then copies to PSRAM cache |
| `$E0` | Read Track | Returns immediate success (not fully emulated) |
| `$F0` | Write Track (Format) | Parses raw format stream, extracts sector data, writes to PSRAM cache. Used by DSKINI |
| `$D0` | Force Interrupt | Bit 3 = immediate INTRQ |

**INTRQ/HALT/NMI flow** (critical for DSKCON compatibility):
1. Command completes (or sector transfer finishes) -> `set_intrq(true)`
2. INTRQ asserted -> HALT disabled, CPU released
3. NMI fires if `!density && intrq` (density gating after XOR)
4. INTRQ cleared on status read (`$FF48`) or new command write
5. Sector transfers use `intrq_defer = 1` — fires INTRQ one tick later so CPU can store the last byte
6. `sv_disk_tick()` must be called once per CPU instruction to handle deferred INTRQ

**DRQ/HALT flow** (byte-level sync):
- DRQ high -> release CPU halt
- DRQ low + halt_enable -> halt CPU (waits for next byte)

**PSRAM caching:**
- Entire disk image loaded into PSRAM at mount time (~161KB for standard 35T/18S)
- All sector reads/writes go to in-memory cache (zero SD latency during emulation)
- SD -> PSRAM uses 512-byte bounce buffer (ESP32 SPI DMA can't write directly to PSRAM)
- Dirty images flushed back to SD on eject, explicit F key in Disk Manager, or auto-flush on F2 reset
- Up to 4 drives (~644KB PSRAM total)

**Geometry detection** (`sv_disk_detect_geometry`):
- VDK files: 12-byte header
- JVC/DSK files: header = `file_size % 256`
- Track count derived from data size / (sectors_per_track * sector_size)
- >80 tracks assumed double-sided

### sv_debug.cpp — Debug Memory Dump

Dumps emulated CoCo memory to the serial port in **Motorola S-Record** or **Intel HEX** format. Reads memory via `machine_read()`, so the output reflects the live SAM/ROM mapping (exactly what the 6809 CPU sees).

**UI fields** (navigate with Up/Down):

| Field | Controls | Description |
|---|---|---|
| Format | Left/Right | Toggle between `S-Record` and `Intel HEX` |
| Start Address | 0-9, A-F keys; Left/Right move nibble cursor | 4-digit hex address ($0000-$FFFF) |
| End Address | Same as above | 4-digit hex address ($0000-$FFFF) |
| Send Dump | Enter | Executes the dump to serial |

**Key bindings in debug screen:**

| Key | Code | Action |
|---|---|---|
| Up/Down | `0x52/0x51` | Move between fields |
| Left/Right | `0x50/0x4F` | Toggle format / move hex cursor |
| 0-9, A-F | `0x27,0x1E-0x26,0x04-0x09` | Enter hex digit at cursor position |
| Enter | `0x28` | Execute dump (when on "Send Dump" field) |
| ESC | `0x29` | Return to main menu |

**Output format:** A header banner is printed before the hex data:

```
========================================
  CoCo ESP32 MEMORY DUMP
  Format : Motorola S-Record
  Range  : $3C00 - $3C0F (16 bytes)
========================================
S00B0000434F434F44554D5091
S1133C008634B7FF21B7FF01B7FF03B7FF237E62xx
S9033C00xx
========== END OF DUMP =================
```

**Record details:**
- S-Record: S0 header (module name "COCODUMP"), S1 data records (16 bytes/line), S9 end record with exec address
- Intel HEX: Data records (16 bytes/line), EOF record (`:00000001FF`)
- Both formats include proper checksums
- 16 bytes per record line (`DUMP_BYTES_PER_LINE`)
- Addresses that would wrap past $FFFF are handled safely

**State struct** (`SV_DebugState` in `sv_debug.h`):
- `active_field` — which UI field has focus
- `format` — `SV_DUMP_SREC` or `SV_DUMP_IHEX`
- `start_addr` / `end_addr` — 16-bit addresses (swapped automatically if reversed)
- `hex_digit` — nibble cursor position (0-3) within address field
- `dumping` — true during serial output (shows "Dumping..." on screen)

### sv_render.cpp — Rendering Engine

Renders directly to `TFT_eSPI` using built-in Font 2 (16px). All coordinates fit inside the emulator sprite area `(32,24)-(288,216)` = 256x192 pixels.

**Layout constants:**
```
SV_BORDER_X=32  SV_BORDER_Y=24  (top-left of OSD area)
SV_BORDER_W=256  SV_BORDER_H=192
SV_TITLE_H=18   SV_ITEM_H=18   SV_FOOTER_H=16
SV_CONTENT_X = BORDER_X + 8
SV_CONTENT_Y = BORDER_Y + TITLE_H + 4
```

**Color palette** (RGB565) — blue theme:
| Constant | Color | Hex | Used for |
|---|---|---|---|
| `SV_COLOR_BG` | Dark blue | `0x0008` | Background |
| `SV_COLOR_TEXT` | Bright blue-white | `0x5DDF` | Normal text |
| `SV_COLOR_HIGHLIGHT` | Bright blue-white | `0x5DDF` | Selected item bg |
| `SV_COLOR_HL_TEXT` | Dark blue | `0x0008` | Selected item text |
| `SV_COLOR_DIM` | Dim blue | `0x3B3F` | Footer, scrollbar |
| `SV_COLOR_DIR` | Yellow | `0xFFE0` | Directory icon |
| `SV_COLOR_DISK` | Cyan | `0x07FF` | Disk image icon |
| `SV_COLOR_WARN` | Red | `0xF800` | Unsupported file icon |

**Rendering functions:**
- `sv_render_frame(title, footer)` — clears area, draws border, title bar (inverse), footer with separator line
- `sv_render_menu_item(index, label, value, highlighted)` — single row with `>` cursor, optional right-aligned value
- `sv_render_file_entry(index, name, size, is_dir, is_supported, highlighted)` — file row with icon (`+`=dir, `*`=disk, `!`=unsupported) and size
- `sv_render_scrollbar(start, visible, total)` — vertical scrollbar thumb
- `sv_render_confirm_dialog(message, yes_highlighted)` — centered Yes/No dialog box
- `sv_render_status_line(text, color)` — centered status at bottom of content area

---

## Persistence (ESP32 Preferences)

Namespace: `"sv"`. Stored in NVS flash.

| Key | Type | Description |
|---|---|---|
| `last_dir` | String | Last browsed directory path |
| `d0_path` .. `d3_path` | String | Mounted disk paths per drive |
| `auto_mount` | Bool | Auto-mount on boot (default true) |

- `supervisor_save_state()` — writes current dir + all mounted disk paths
- `supervisor_load_state()` — restores dir + auto-mounts disks on boot
- `supervisor_quick_mount_last_disk()` — mounts drive 0 from saved path (used at boot)

---

## NMI/HALT Wiring

Set up in `supervisor_init()`:

```cpp
m->fdc.nmi_callback = [](void* ctx) {
    mc6809_nmi(&((Machine*)ctx)->cpu);
};
m->fdc.halt_callback = [](void* ctx, bool halted) {
    ((Machine*)ctx)->cpu.halted = halted;
};
m->fdc.callback_context = m;
```

This connects the FDC's interrupt/halt signals to the MC6809 CPU through the Machine struct.

---

## Key Bindings (HID Usage Codes)

| Key | Code | Context |
|---|---|---|
| F1 | `0x3A` | Toggle OSD on/off (global) |
| Up | `0x52` | Move cursor up |
| Down | `0x51` | Move cursor down |
| Enter | `0x28` | Select / confirm (disk mgr: eject if mounted, mount if empty) |
| ESC | `0x29` | Back / cancel |
| Backspace | `0x2A` | Parent directory (file browser) |
| PgUp | `0x4B` | Page up (file browser) |
| PgDn | `0x4E` | Page down (file browser) |
| Home | `0x4A` | Jump to first (file browser) |
| End | `0x4D` | Jump to last (file browser) |
| M | `0x10` | Mount (disk manager) |
| E | `0x08` | Eject (disk manager) |
| F | `0x09` | Flush selected drive to SD (disk manager) |
| F2 | `0x3B` | Soft reset — auto-flushes all dirty disks before reset |
| Left/Right | `0x50/0x4F` | Toggle Yes/No (confirm dialog); toggle format / move nibble (debug dump) |
| 0-9 | `0x27,0x1E-0x26` | Hex digit input (debug dump address fields) |
| A-F | `0x04-0x09` | Hex digit input (debug dump address fields) |

---

## Known Limitations & TODOs

- **No framebuffer snapshot:** TFT has no MISO wired, so `readRect()` is impossible. On OSD close, the emulator simply repaints on the next frame (brief black flash in border area).
- **Machine Select:** `SV_MACHINE_SELECT` / `SV_ACT_MACHINE_SELECT` are stubs (always "CoCo 2").
- **Settings screen:** `SV_SETTINGS` state exists but has no implementation.
- **DMK format:** Recognized in file browser but not mountable (no DMK parser).
- **menu_cursor reuse:** Main menu and disk manager share `sv.menu_cursor`, so transitioning between them resets cursor position manually.
- **Static val_buf in disk_manager_render:** Uses `static char val_buf[32]` — works because only one item renders at a time, but fragile if rendering changes.
- **Max 128 file entries:** `SV_FB_MAX_ENTRIES = 128` — directories with more entries are truncated.
- **Single-SPI bus:** SD reads and TFT writes cannot overlap. File browser scans directory BEFORE rendering.

---

## Adding a New Menu Screen (Checklist)

1. Add a new `SV_State` value in `supervisor.h`
2. Add a new `SV_MenuAction` in `sv_menu.h` and a menu item in `sv_menu.cpp`
3. Write `execute_action()` case to transition: set `prev_state`, change `state`, set `needs_redraw`
4. Write `your_screen_on_key()` handler — dispatch in `supervisor_on_key()` switch
5. Write `your_screen_render()` using `sv_render_frame()` + `sv_render_menu_item()` — dispatch in `supervisor_update_and_render()` switch
6. Handle ESC to return to `SV_MAIN_MENU` (restore cursor position if needed)
