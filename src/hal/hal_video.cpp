/*
 * ============================================================
 *   CoCo_ESP32 Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : hal_video.cpp
 *  Module : Video HAL — MC6847 palette output to ILI9341 TFT via TFT_eSPI with frame skipping
 * ============================================================
 */

/*
 * hal_video.cpp - Video output via TFT_eSPI
 *
 * Renders MC6847 VDG output (256x192 palette indices) to SPI TFT.
 *
 * Two scale modes (DISPLAY_SCALE_MODE in config.h):
 *   0 = 1:1 centered (256x192 sprite, black borders)
 *   1 = Scaled fill  (nearest-neighbor stretch to DISPLAY_WIDTH x DISPLAY_HEIGHT)
 *
 * To reduce SPI blocking time (which stalls CPU emulation and
 * causes audio pauses), only every Nth frame is pushed to TFT.
 */

#include "hal.h"
#include "../utils/debug.h"
#include "../core/mc6847.h"

#include <TFT_eSPI.h>

static TFT_eSPI tft;
static TFT_eSprite* sprite = nullptr;
static bool display_available = false;

// --- OPT-16: VRAM shadow compare to skip SPI push on unchanged screen ---
// VRAM size per GM value: bytes_per_row * rows (from mc6847.cpp graphics table)
static const uint16_t vram_sizes_by_gm[8] = {
    1024, 1024, 2048, 1536, 3072, 3072, 6144, 6144
};
static const uint16_t VRAM_TEXT_SIZE = 512;    // Text mode: 32 cols x 16 rows
static const uint16_t VRAM_SHADOW_MAX = 6144;  // Largest mode (RG6/CG6)
static uint8_t  vram_shadow[VRAM_SHADOW_MAX];
static uint16_t shadow_base = 0xFFFF;          // Force first push
static uint8_t  shadow_mode = 0xFF;
static uint8_t  force_push_count = 0;          // Force N pushes after mode/base change

// RGB565 palette mapping for VDG color indices
static uint16_t palette_rgb565[16];
// Byte-swapped palette for direct framebuffer writes (modes 1 & 2)
static uint16_t palette_swapped[16];

// --- Scale mode configuration ---

#if DISPLAY_SCALE_MODE == 1
  // --- Mode 1: Scaled fill (stretch to display minus border) ---
  static const int SPRITE_W = DISPLAY_WIDTH;
  static const int SPRITE_H = DISPLAY_HEIGHT;
  static const int SPR_X = 0;
  static const int SPR_Y = 0;
  static const int INNER_X = DISPLAY_BORDER;
  static const int INNER_Y = DISPLAY_BORDER;
  static const int INNER_W = DISPLAY_WIDTH  - DISPLAY_BORDER * 2;
  static const int INNER_H = DISPLAY_HEIGHT - DISPLAY_BORDER * 2;

#elif DISPLAY_SCALE_MODE == 2
  // --- Mode 2: Zoom centered (fixed zoom factor, centered, clipped to display) ---
  // Zoomed pixel dimensions (clamp to display size)
  #define _ZOOM_W_RAW  (VDG_ACTIVE_WIDTH  * DISPLAY_ZOOM_X10 / 10)
  #define _ZOOM_H_RAW  (VDG_ACTIVE_HEIGHT * DISPLAY_ZOOM_X10 / 10)
  #define _MIN(a,b) ((a)<(b)?(a):(b))
  static const int SPRITE_W = _MIN(_ZOOM_W_RAW, DISPLAY_WIDTH);
  static const int SPRITE_H = _MIN(_ZOOM_H_RAW, DISPLAY_HEIGHT);
  static const int SPR_X = (DISPLAY_WIDTH  - SPRITE_W) / 2;
  static const int SPR_Y = (DISPLAY_HEIGHT - SPRITE_H) / 2;
  static const int INNER_X = 0;
  static const int INNER_Y = 0;
  static const int INNER_W = SPRITE_W;
  static const int INNER_H = SPRITE_H;

#else
  // --- Mode 0: 1:1 centered (native 256x192) ---
  static const int SPRITE_W = VDG_ACTIVE_WIDTH;
  static const int SPRITE_H = VDG_ACTIVE_HEIGHT;
  static const int SPR_X = (DISPLAY_WIDTH - VDG_ACTIVE_WIDTH) / 2;
  static const int SPR_Y = (DISPLAY_HEIGHT - VDG_ACTIVE_HEIGHT) / 2;
#endif

// Shared scale infrastructure for modes 1 and 2
#if DISPLAY_SCALE_MODE == 1 || DISPLAY_SCALE_MODE == 2
  static uint16_t x_map[DISPLAY_WIDTH];
  static uint16_t y_start[VDG_ACTIVE_HEIGHT];
  static uint8_t  y_count[VDG_ACTIVE_HEIGHT];
  static uint16_t scaled_line[DISPLAY_WIDTH];

  static void init_scale_tables() {
      for (int dx = 0; dx < INNER_W; dx++) {
          x_map[dx] = (uint16_t)(dx * VDG_ACTIVE_WIDTH / INNER_W);
      }
      for (int vy = 0; vy < VDG_ACTIVE_HEIGHT; vy++) {
          y_start[vy] = (uint16_t)(INNER_Y + vy * INNER_H / VDG_ACTIVE_HEIGHT);
          uint16_t y_end = (uint16_t)(INNER_Y + (vy + 1) * INNER_H / VDG_ACTIVE_HEIGHT);
          y_count[vy] = (uint8_t)(y_end - y_start[vy]);
      }
  }
#endif

static void init_palette() {
    // MC6847 palette derived from XRoar's "ideal" NTSC VDG data sheet values.
    // These match the actual composite video output of a real MC6847.
    palette_rgb565[VDG_COLOR_GREEN]          = 0x0FE1;  // (10, 255, 10)
    palette_rgb565[VDG_COLOR_YELLOW]         = 0xFFE8;  // (255, 255, 67)
    palette_rgb565[VDG_COLOR_BLUE]           = 0x20B6;  // (34, 20, 180)
    palette_rgb565[VDG_COLOR_RED]            = 0xB024;  // (182, 5, 34)
    palette_rgb565[VDG_COLOR_WHITE]          = 0xFFFF;  // (255, 255, 255) Buff
    palette_rgb565[VDG_COLOR_CYAN]           = 0x0EAE;  // (10, 212, 112)
    palette_rgb565[VDG_COLOR_MAGENTA]        = 0xF8FF;  // (255, 28, 255)
    palette_rgb565[VDG_COLOR_ORANGE]         = 0xFA01;  // (255, 66, 10)
    palette_rgb565[VDG_COLOR_BLACK]          = 0x0841;  // (9, 9, 9)
    palette_rgb565[VDG_COLOR_DARK_GREEN]     = 0x0200;  // (0, 65, 0)
    palette_rgb565[VDG_COLOR_DARK_ORANGE]    = 0x6800;  // (108, 0, 0)
    palette_rgb565[VDG_COLOR_BRIGHT_ORANGE]  = 0xFDA8;  // (255, 180, 67)

    // Pre-swap bytes for direct framebuffer writes
    for (int i = 0; i < 16; i++) {
        uint16_t c = palette_rgb565[i];
        palette_swapped[i] = (c >> 8) | (c << 8);
    }
}

void hal_video_init(void) {
    DEBUG_PRINT("  Video: TFT_eSPI init...");

    tft.init();

#if DISPLAY_TYPE == 3
    // ST7796 480x320: rotation 1 = landscape (480 wide x 320 tall)
    tft.setRotation(1);
#else
    // ST7789/ILI9341 320x240: rotation 1 = landscape
    tft.invertDisplay(false);
    tft.setRotation(1);
#endif

    tft.fillScreen(TFT_BLACK);

#if PIN_TFT_BL >= 0
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);
#endif

    init_palette();

#if DISPLAY_SCALE_MODE == 1
    init_scale_tables();
    DEBUG_PRINTF("  Video: scale mode FILL (%dx%d -> %dx%d, border=%d)",
                 VDG_ACTIVE_WIDTH, VDG_ACTIVE_HEIGHT,
                 INNER_W, INNER_H, DISPLAY_BORDER);
#elif DISPLAY_SCALE_MODE == 2
    init_scale_tables();
    DEBUG_PRINTF("  Video: scale mode ZOOM x%d.%d (%dx%d sprite at (%d,%d))",
                 DISPLAY_ZOOM_X10 / 10, DISPLAY_ZOOM_X10 % 10,
                 SPRITE_W, SPRITE_H, SPR_X, SPR_Y);
#else
    DEBUG_PRINTF("  Video: scale mode 1:1 centered at (%d,%d)", SPR_X, SPR_Y);
#endif

    sprite = new TFT_eSprite(&tft);
    sprite->setColorDepth(16);
    sprite->setAttribute(PSRAM_ENABLE, true);

    void* sptr = sprite->createSprite(SPRITE_W, SPRITE_H);
    if (!sptr) {
        DEBUG_PRINT("  Video: PSRAM sprite failed, trying heap...");
        sprite->setAttribute(PSRAM_ENABLE, false);
        sptr = sprite->createSprite(SPRITE_W, SPRITE_H);
    }

    if (sptr) {
        display_available = true;
        sprite->fillSprite(TFT_BLACK);
        DEBUG_PRINTF("  Video: sprite OK at %p (%d bytes)", sptr,
                     SPRITE_W * SPRITE_H * 2);
    } else {
        DEBUG_PRINT("  Video: sprite alloc failed, running headless");
    }
}

void hal_video_set_mode(uint8_t mode) {
    (void)mode;
}

void hal_video_render_scanline(int line, const uint8_t* pixels, int width) {
    if (!display_available || !sprite || !pixels) return;
    if (line < 0 || line >= VDG_ACTIVE_HEIGHT) return;

#if DISPLAY_SCALE_MODE == 1 || DISPLAY_SCALE_MODE == 2
    // Build one scaled row: stretch 256 VDG pixels -> INNER_W
    // Uses pre-swapped palette for direct framebuffer writes
    int w = (width < VDG_ACTIVE_WIDTH) ? width : VDG_ACTIVE_WIDTH;
    for (int dx = 0; dx < INNER_W; dx++) {
        int sx = x_map[dx];
        scaled_line[dx] = (sx < w) ? palette_swapped[pixels[sx] & 0x0F] : 0;
    }
    // Write scaled row directly into sprite framebuffer
    uint16_t* fb = (uint16_t*)sprite->getPointer();
    if (fb) {
        for (int r = 0; r < y_count[line]; r++) {
            int dy = y_start[line] + r;
            memcpy(&fb[dy * SPRITE_W + INNER_X], scaled_line, INNER_W * sizeof(uint16_t));
        }
    }
#else
    // 1:1: write directly into sprite framebuffer (avoids drawPixel overhead)
    int w = (width < VDG_ACTIVE_WIDTH) ? width : VDG_ACTIVE_WIDTH;
    uint16_t* fb = (uint16_t*)sprite->getPointer();
    if (fb) {
        uint16_t* row = &fb[line * SPRITE_W];
        for (int x = 0; x < w; x++) {
            row[x] = palette_swapped[pixels[x] & 0x0F];
        }
    }
#endif
}

// FPS overlay
static bool fps_overlay_enabled = false;
static uint32_t fps_frame_count = 0;
static uint32_t fps_last_time = 0;
static float fps_value = 0.0f;

// Count emulated frames and update FPS value (called every frame)
static void fps_update(void) {
    fps_frame_count++;
    uint32_t now = millis();
    uint32_t elapsed = now - fps_last_time;
    if (elapsed >= 1000) {
        fps_value = (float)fps_frame_count * 1000.0f / (float)elapsed;
        fps_frame_count = 0;
        fps_last_time = now;
    }
}

// Draw FPS text on TFT (called only on display push)
static void fps_overlay_draw(void) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", fps_value);
    tft.setTextFont(2);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(buf, 2, 2);
}

void hal_video_force_repaint(void) {
    force_push_count = 3;
}

void hal_video_toggle_fps_overlay(void) {
    fps_overlay_enabled = !fps_overlay_enabled;
    if (!fps_overlay_enabled && display_available) {
        // Clear the FPS text area when turning off
        tft.fillRect(0, 0, 50, 16, TFT_BLACK);
    }
    fps_frame_count = 0;
    fps_last_time = millis();
}

void hal_video_present(const uint8_t* ram, uint16_t vdg_base, uint8_t vdg_mode) {
    if (!display_available || !sprite) return;

    // Count every emulated frame for accurate FPS
    if (fps_overlay_enabled) {
        fps_update();
    }

    // OPT-16: VRAM shadow compare — skip SPI push if screen unchanged
    // Determine VRAM region size based on current mode
    uint16_t vram_size;
    if (vdg_mode & VDG_AG) {
        // Graphics mode: size depends on GM bits (0-7)
        uint8_t gm = vdg_mode & 0x07;
        vram_size = vram_sizes_by_gm[gm];
    } else {
        // Text/semigraphics mode: 32x16 = 512 bytes
        vram_size = VRAM_TEXT_SIZE;
    }

    // Detect mode or base change — force several pushes to catch
    // multi-frame screen setup (e.g., game title screens drawn over 2-3 frames)
    bool mode_or_base_changed = (vdg_base != shadow_base || vdg_mode != shadow_mode);
    if (mode_or_base_changed) {
        force_push_count = 10;  // Force next 10 frames to push
    }

    // Check if anything changed: base address, mode bits, or VRAM content
    if (ram && force_push_count == 0 &&
        memcmp(vram_shadow, ram + vdg_base, vram_size) == 0) {
        return;  // No change — skip SPI push
    }

    if (force_push_count > 0) force_push_count--;

    // Update shadow
    if (ram) {
        memcpy(vram_shadow, ram + vdg_base, vram_size);
        shadow_base = vdg_base;
        shadow_mode = vdg_mode;
    }

    sprite->pushSprite(SPR_X, SPR_Y);
    if (fps_overlay_enabled) {
        fps_overlay_draw();
    }
}

// Expose TFT instance for supervisor OSD rendering
TFT_eSPI* hal_video_get_tft(void) {
    return display_available ? &tft : nullptr;
}
