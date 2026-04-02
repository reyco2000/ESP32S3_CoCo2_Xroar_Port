# Video Subsystem — ESP32_CoCo2_XRoar_Port

## Overview

The video pipeline renders the MC6847 VDG (Video Display Generator) output to an SPI TFT display using the TFT_eSPI library. The VDG produces 256×
192 palette-indexed scanlines; the HAL converts these to RGB565 pixels and pushes them to the TFT via a sprite framebuffer.

## Hardware

| Parameter | Value |
|-----------|-------|
| Display types | ILI9341 (type 0), ST7789 (type 1), ST7796 (type 3) |
| Current config | ST7789, 320×240 (`DISPLAY_TYPE=1`) |
| SPI pins | CS=10, DC=2, RST=4, MOSI=11, SCLK=12 |
| Backlight | GPIO 5 (`PIN_TFT_BL`) |
| SPI speed | 40 MHz |
| Library | TFT_eSPI (configured via `User_Setup.h`) |

The ST7796 variant provides 480×320 resolution. All pin assignments and display type are in `config.h`.

## Pipeline

```
MC6847 VDG          Machine Loop            HAL Video             TFT
─────────────      ──────────────          ──────────────        ─────
line_buffer[256] → hal_video_render_scanline() → sprite FB → pushSprite()
(palette indices)   (called per active line)     (RGB565)     (on present)
```

### Per-Scanline Flow

1. `machine_run_scanline()` runs CPU cycles for one scanline
2. For active scanlines (0–191), `mc6847_render_scanline()` fills `vdg.line_buffer[]` with palette indices (0–11)
3. `hal_video_render_scanline(line, pixels, width)` converts indices to RGB565 and writes into the sprite framebuffer
4. After all 262 scanlines, `hal_video_present(ram, vdg_base, vdg_mode)` performs VRAM shadow compare and pushes the sprite to TFT only if the screen changed

### Frame Timing and VRAM Shadow Compare (OPT-16)

- 262 scanlines per frame (NTSC), 192 active
- `hal_video_present()` implements **VRAM shadow compare** (OPT-16, 2026-03-26): before pushing, `memcmp()` compares the current CoCo VRAM region against a 6,144-byte shadow buffer. If VRAM content, VDG mode, and SAM display base are all unchanged, the SPI push is skipped entirely
- On mode or base change, a 10-frame force-push window ensures multi-frame screen setup (e.g., game title screens) is fully captured
- `hal_video_force_repaint()` sets `force_push_count = 3` to force TFT pushes on demand — called when the supervisor OSD closes so the emulated display repaints over OSD residue (fixes blank screen after F1 toggle under OS-9 or any static-screen scenario)
- **Replaces the old blind `FRAME_SKIP=1`** (which pushed every 2nd frame regardless of changes) with intelligent dirty detection — dirty frames push immediately, unchanged frames are free
- **Measured performance** (2026-03-26): 64 FPS text mode (static), 45 FPS graphics (static), 27 FPS graphics (scrolling VRAM)

VRAM region sizes by mode:
| Mode | VRAM bytes | memcmp cost |
|------|-----------|-------------|
| Text (32×16) | 512 | ~0.5 us |
| CG1-RG1 (GM 0-1) | 1,024 | ~1 us |
| CG2 (GM 2) | 2,048 | ~2 us |
| RG2 (GM 3) | 1,536 | ~1.5 us |
| CG3-RG3 (GM 4-5) | 3,072 | ~3 us |
| CG6-RG6 (GM 6-7) | 6,144 | ~5 us |

## Display Scale Modes

Configured via `DISPLAY_SCALE_MODE` in `config.h`. Currently set to **mode 0**.

### Mode 0: 1:1 Centered (default)

- Sprite size: 256×192 (native VDG resolution)
- Centered on 320×240 display with black borders
- Sprite position: `((320-256)/2, (240-192)/2)` = (32, 24)
- Fastest mode — no scaling math, direct palette lookup per pixel

### Mode 1: Scaled Fill

- Sprite size: full display (320×240 or 480×320)
- VDG 256×192 is nearest-neighbor stretched to `(DISPLAY_WIDTH - 2*BORDER)` × `(DISPLAY_HEIGHT - 2*BORDER)`
- `DISPLAY_BORDER=12` → inner area is 296×216
- Uses pre-computed lookup tables (`x_map[]`, `y_start[]`, `y_count[]`) for fast scaling

### Mode 2: Zoom Centered

- Fixed zoom factor: `DISPLAY_ZOOM_X10 / 10` (default 1.7×)
- Zoomed size: `256*1.7=435`, `192*1.7=326` → clamped to display bounds
- Centered with black borders, clipped if zoom exceeds display
- Uses same lookup-table infrastructure as mode 1

## Scaling Implementation

Modes 1 and 2 share pre-computed tables built at init:

- **`x_map[dx]`** — for each destination X pixel, which source VDG column to sample
- **`y_start[vy]`** / **`y_count[vy]`** — for each VDG source line, the starting destination row and how many destination rows it maps to

Per scanline, `hal_video_render_scanline()`:
1. Builds `scaled_line[]` — one row of INNER_W pixels using `x_map` and the byte-swapped palette
2. Writes `scaled_line` into the sprite framebuffer for each destination row (`y_count[line]` copies via `memcpy`)

## Palette

The MC6847 uses 12 colors (indices 0–11), stored in a 16-entry array. RGB565 values are derived from XRoar's "ideal" NTSC data sheet values:

| Index | Name | RGB565 | Approx RGB |
|-------|------|--------|------------|
| 0 | Green | 0x0FE1 | (10, 255, 10) |
| 1 | Yellow | 0xFFE8 | (255, 255, 67) |
| 2 | Blue | 0x20B6 | (34, 20, 180) |
| 3 | Red | 0xB024 | (182, 5, 34) |
| 4 | White/Buff | 0xFFFF | (255, 255, 255) |
| 5 | Cyan | 0x0EAE | (10, 212, 112) |
| 6 | Magenta | 0xF8FF | (255, 28, 255) |
| 7 | Orange | 0xFA01 | (255, 66, 10) |
| 8 | Black | 0x0841 | (9, 9, 9) |
| 9 | Dark Green | 0x0200 | (0, 65, 0) |
| 10 | Dark Orange | 0x6800 | (108, 0, 0) |
| 11 | Bright Orange | 0xFDA8 | (255, 180, 67) |

Two palette arrays are maintained:
- **`palette_rgb565[16]`** — standard RGB565 values
- **`palette_swapped[16]`** — byte-swapped for direct framebuffer writes (TFT_eSPI stores pixels in big-endian byte order in the sprite buffer)

## Sprite Framebuffer

- Created via `TFT_eSprite` from TFT_eSPI
- 16-bit color depth (RGB565)
- Attempts PSRAM allocation first, falls back to heap
- Size depends on scale mode: 256×192 (mode 0), 320×240 (mode 1), or zoom-dependent (mode 2)
- Memory: `W × H × 2` bytes (e.g., 256×192 = 98,304 bytes)
- Scanline rendering writes directly to the framebuffer via `getPointer()`, bypassing `drawPixel()` for performance

## FPS Overlay

Toggled via `hal_video_toggle_fps_overlay()` (mapped to F5 in supervisor).

- Counts every emulated frame (not just displayed frames) for accurate FPS
- Updates FPS value once per second
- Draws text directly on the TFT (not in the sprite) after `pushSprite()`, so it overlays the border area
- Uses TFT_eSPI font 2 at position (2, 2)

## Key Functions

| Function | Purpose |
|----------|---------|
| `hal_video_init()` | Init TFT, create sprite, build palette and scale tables |
| `hal_video_render_scanline()` | Convert one VDG scanline to RGB565 in sprite FB |
| `hal_video_present(ram, vdg_base, vdg_mode)` | VRAM shadow compare + conditional push to TFT |
| `hal_video_set_mode()` | Stub — mode changes handled by VDG/PIA directly |
| `hal_video_force_repaint()` | Force next 3 frames to push (invalidates VRAM shadow) |
| `hal_video_toggle_fps_overlay()` | Toggle FPS counter |
| `hal_video_get_tft()` | Expose TFT instance for supervisor OSD rendering |

## Files

- `ESP32_CoCo2_XRoar_Port/config.h` — display type, resolution, scale mode, pins, SPI speed
- `ESP32_CoCo2_XRoar_Port/src/hal/hal_video.cpp` — all video HAL implementation
- `ESP32_CoCo2_XRoar_Port/src/hal/hal.h` — HAL interface declarations
- `ESP32_CoCo2_XRoar_Port/src/core/mc6847.h` — VDG constants and structure
- `ESP32_CoCo2_XRoar_Port/src/core/mc6847.cpp` — VDG scanline rendering (palette index output)
- `ESP32_CoCo2_XRoar_Port/src/core/machine.cpp` — frame loop calling render_scanline + present
