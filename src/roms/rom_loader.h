/*
 * ============================================================
 *   CoCo_ESP32 Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : rom_loader.h
 *  Module : ROM loader interface — Dragon/CoCo ROM loading and CRC validation
 * ============================================================
 */

/*
 * rom_loader.h - ROM loading utilities
 *
 * Handles loading ROM images from storage and validating them via CRC.
 */

#ifndef ROM_LOADER_H
#define ROM_LOADER_H

#include <Arduino.h>

// Known ROM CRCs (from XRoar's crclist.c)
#define CRC_BAS13       0xD8F4D15E  // Color BASIC 1.3
#define CRC_BAS12       0x54A5847A  // Color BASIC 1.2
#define CRC_EXTBAS11    0xA82A6254  // Extended Color BASIC 1.1
#define CRC_EXTBAS10    0x6B7A269E  // Extended Color BASIC 1.0
#define CRC_DISK11      0x0B9C914A  // Disk BASIC 1.1
#define CRC_DISK10      0xC3525CF8  // Disk BASIC 1.0

// CRC-32 for ROM validation
uint32_t rom_crc32(const uint8_t* data, size_t len);

// Validate ROM by checking CRC against known values
// Returns true if CRC matches a known ROM image
bool rom_validate(const uint8_t* data, size_t len, const char* name);

#endif // ROM_LOADER_H
