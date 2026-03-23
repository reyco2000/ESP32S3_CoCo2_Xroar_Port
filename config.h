/*
 * ============================================================
 *   CoCo_ESP32 Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : config.h
 *  Module : Hardware configuration — pin assignments, compile-time options, and build constants
 * ============================================================
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================
// Build options
// ============================================================

// Machine type: 0 = Dragon 32, 1 = Dragon 64, 2 = CoCo 1, 3 = CoCo 2
#define MACHINE_TYPE            3

// CPU variant: 0 = MC6809, 1 = HD6309
#define CPU_VARIANT             0

// RAM size in KB (16, 32, or 64)
#define RAM_SIZE_KB             64

// Enable debug output on Serial
#define DEBUG_ENABLED           1

// Target frames per second (NTSC=60, PAL=50)
#define TARGET_FPS              60

// ============================================================
// Display configuration
// ============================================================

// Display type: 0 = ILI9341 SPI (320x240)
//               1 = ST7789 SPI  (320x240)
//               2 = Composite out
//               3 = ST7796 SPI  (480x320)
#define DISPLAY_TYPE            1

// Display resolution derived from display type
#if DISPLAY_TYPE == 3
  #define DISPLAY_WIDTH         480
  #define DISPLAY_HEIGHT        320
#else
  #define DISPLAY_WIDTH         320
  #define DISPLAY_HEIGHT        240
#endif

// Display scale mode:
//   0 = 1:1 centered (256x192 native, black borders)
//   1 = Scaled fill  (nearest-neighbor stretch to fill display)
//   2 = Zoom centered (integer/fractional zoom, centered with black borders)
#define DISPLAY_SCALE_MODE      0

// Border size in pixels (applies to scale mode 1 only)
// Set to 0 for no border, 10-15 for a CoCo-style black frame
#define DISPLAY_BORDER          12

// Zoom factor x10 (applies to scale mode 2 only)
// Examples: 10 = 1.0x (same as mode 0), 15 = 1.5x, 20 = 2.0x
// Image is clipped if it exceeds display size
#define DISPLAY_ZOOM_X10        17

// SPI display pins
#define PIN_TFT_CS              10
#define PIN_TFT_DC              2
#define PIN_TFT_RST             4
#define PIN_TFT_MOSI            11
#define PIN_TFT_SCLK            12
#define PIN_TFT_MISO            -1
#define PIN_TFT_BL              5    // Backlight (-1 to disable)

// SPI speed for display (Hz)
#define TFT_SPI_FREQ            40000000

// ============================================================
// Audio configuration
// ============================================================

// Audio output method: 0 = DAC (GPIO17), 1 = I2S, 2 = disabled
#define AUDIO_OUTPUT            0

// Audio sample rate
#define AUDIO_SAMPLE_RATE       22050

// Audio buffer size (samples)
#define AUDIO_BUFFER_SIZE       512

// I2S pins (if AUDIO_OUTPUT == 1)
//#define PIN_I2S_BCLK            26
//#define PIN_I2S_LRCLK           25
//#define PIN_I2S_DOUT            22

// DAC pin (if AUDIO_OUTPUT == 0, ESP32 DAC on GPIO25 or GPIO26)
#define PIN_DAC_OUT             17

// ============================================================
// Input configuration
// ============================================================

// USB keyboard
//#define PIN_PS2_DATA            16
//#define PIN_PS2_CLK             17

// Joystick analog pins (directly connected pots or ADC)
#define PIN_JOY0_X              9 
#define PIN_JOY0_Y              8
#define PIN_JOY0_BTN            18

#define PIN_JOY1_X              16
#define PIN_JOY1_Y              15    // ADC1_CH3 (VN)
#define PIN_JOY1_BTN            7

// ============================================================
// Storage configuration
// ============================================================

// Storage type: 0 = SD card (SPI), 1 = SPIFFS, 2 = LittleFS
#define STORAGE_TYPE            0

// SD card SPI pins (dedicated bus — NOT shared with TFT)
#define PIN_SD_CS               38
#define PIN_SD_MOSI             40
#define PIN_SD_MISO             41
#define PIN_SD_SCLK             39

// ROM file paths (on SD or SPIFFS)
#define ROM_BASE_PATH           "/roms"
#define ROM_BASIC_FILE          "bas13.rom"
#define ROM_EXT_BASIC_FILE      "extbas11.rom"
#define ROM_DISK_FILE           "disk11.rom"

// ============================================================
// Memory layout
// ============================================================

// Use PSRAM for emulated RAM if available
#define USE_PSRAM               1

// CoCo memory map sizes
#define COCO_RAM_SIZE           (RAM_SIZE_KB * 1024)
#define COCO_ROM_SIZE           (32 * 1024)      // Max total ROM space

// Video RAM is part of main RAM, mapped by SAM
// MC6847 active display: 6144 bytes (text) or 6144 bytes (graphics)
#define VRAM_SIZE               6144

// ============================================================
// Timing
// ============================================================

// MC6809 clock: 0.895 MHz (NTSC) or 0.8856 MHz (PAL)
#define CPU_CLOCK_HZ            895000

// Cycles per video frame
#define CYCLES_PER_FRAME        (CPU_CLOCK_HZ / TARGET_FPS)

// Scanlines per frame (NTSC=262, PAL=312)
#define SCANLINES_PER_FRAME     262

// Active display scanlines
#define ACTIVE_SCANLINES        192

#endif // CONFIG_H
