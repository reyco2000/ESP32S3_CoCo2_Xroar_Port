/*
 * ============================================================
 *   CoCo_ESP32 Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : debug.h
 *  Module : Debug output macros — compile-time Serial.printf wrappers (DEBUG_LOG, etc.)
 * ============================================================
 */

/*
 * debug.h - Debug output macros
 */

#ifndef DEBUG_H
#define DEBUG_H

#include "../../config.h"

#if DEBUG_ENABLED
    #define DEBUG_PRINT(msg)          Serial.println(msg)
    #define DEBUG_PRINTF(fmt, ...)    Serial.printf(fmt "\n", ##__VA_ARGS__)
    #define DEBUG_TODO(func)          Serial.printf("TODO: %s() not implemented\n", func)
#else
    #define DEBUG_PRINT(msg)
    #define DEBUG_PRINTF(fmt, ...)
    #define DEBUG_TODO(func)
#endif

#endif // DEBUG_H
