/*
 * ============================================================
 *   ESP32_CoCo2_XRoar_Port Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : ESP32_CoCo2_XRoar_Port.ino
 *  Module : Main Arduino sketch — setup/loop entry point for CoCo 2 emulator on ESP32-S3
 * ============================================================
 */

#include "config.h"
#include "src/core/machine.h"
#include "src/hal/hal.h"
#include "src/hal/usb_kbd_host.h"
#include "src/supervisor/supervisor.h"
#include "src/utils/debug.h"

// Uncomment to enable LOADM verification test (serial command 'R' to run)
// #define RUN_INTEGRATION_TESTS 1

#ifdef RUN_INTEGRATION_TESTS
#include "src/tests/integration_test.h"
#endif

Machine coco;

void setup() {
    Serial.begin(115200);
    delay(500);

    DEBUG_PRINT("=================================");
    DEBUG_PRINT("CoCo_ESP32 - Starting up...");
    DEBUG_PRINTF("CPU freq: %d MHz", ESP.getCpuFreqMHz());
    DEBUG_PRINT("----- Memory Report -----");
    DEBUG_PRINTF("SRAM  total: %d bytes", ESP.getHeapSize());
    DEBUG_PRINTF("SRAM  free:  %d bytes", ESP.getFreeHeap());
    DEBUG_PRINTF("SRAM  used:  %d bytes", ESP.getHeapSize() - ESP.getFreeHeap());
    DEBUG_PRINTF("SRAM  min free ever: %d bytes", ESP.getMinFreeHeap());
    DEBUG_PRINTF("PSRAM total: %d bytes", ESP.getPsramSize());
    DEBUG_PRINTF("PSRAM free:  %d bytes", ESP.getFreePsram());
    DEBUG_PRINTF("PSRAM used:  %d bytes", ESP.getPsramSize() - ESP.getFreePsram());
    DEBUG_PRINTF("PSRAM min free ever: %d bytes", ESP.getMinFreePsram());
    DEBUG_PRINT("-------------------------");
    DEBUG_PRINT("=================================");

    // Initialize HAL (storage, audio, keyboard, joystick — but NOT video yet)
    hal_init();

    // Initialize emulated machine
    machine_init(&coco);

    // Load ROM images BEFORE video init (SD and TFT share SPI bus)
    if (!machine_load_roms(&coco)) {
        DEBUG_PRINT("WARNING: ROM loading failed - running without ROMs");
    }

    // Now safe to init TFT display (takes over shared SPI bus)
    hal_video_init();

    // Cold reset
    machine_reset(&coco);

    // Initialize supervisor (OSD menu, disk controller, NVS)
    supervisor_init(&coco);
    supervisor_load_state();  // Auto-mount last disks if enabled
    hal_keyboard_set_machine(&coco);

    DEBUG_PRINT("=== Post-Init Memory Report ===");
    DEBUG_PRINTF("SRAM  free:  %d bytes (used: %d)", ESP.getFreeHeap(), ESP.getHeapSize() - ESP.getFreeHeap());
    DEBUG_PRINTF("PSRAM free:  %d bytes (used: %d)", ESP.getFreePsram(), ESP.getPsramSize() - ESP.getFreePsram());
    DEBUG_PRINT("===============================");

    // Wait for USB keyboard to enumerate (up to 3 seconds)
    {
        uint32_t kbd_wait_start = millis();
        while (!hid_host_is_connected() && (millis() - kbd_wait_start) < 3000) {
            delay(50);
        }
        if (hid_host_is_connected()) {
            DEBUG_PRINTF("USB Keyboard connected (%lu ms)", millis() - kbd_wait_start);
        } else {
            DEBUG_PRINT("USB Keyboard not detected (timeout) - will connect when plugged in");
        }
    }

    DEBUG_PRINT("Entering main loop...");

#ifdef RUN_INTEGRATION_TESTS
    Serial.println("\n*** LOADM Verify Test Ready ***");
    Serial.println("1. Mount disk with ZAXXON.BIN");
    Serial.println("2. Type LOADM\"ZAXXON\" on CoCo keyboard + ENTER");
    Serial.println("3. Wait for OK prompt, then send 'R' via serial to verify RAM");
    Serial.println("Commands: R=verify, S=report, D=VRAM hex, T=screen text");
#endif
}

#ifdef RUN_INTEGRATION_TESTS
static IntegrationTest itest(&coco);
#endif

void loop() {
#ifdef RUN_INTEGRATION_TESTS
    // Check for serial test commands
    if (Serial.available()) {
        char c = Serial.read();
        if (c == 'R' || c == 'r' || c == 'S' || c == 's' ||
            c == 'D' || c == 'd' || c == 'T' || c == 't') {
            itest.process_serial_command(c);
        }
    }
#endif

    // Process host input (keyboard, joystick — includes F1 intercept)
    hal_process_input();

    // Check if supervisor is handling this frame
    if (supervisor_update_and_render()) {
        // Supervisor is active — emulation paused
        yield();
        return;
    }

    // Run one video frame worth of emulation
    machine_run_frame(&coco);

    // Push framebuffer to display
    hal_render_frame();
}
