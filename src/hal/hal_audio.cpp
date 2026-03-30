/*
 * ============================================================
 *   CoCo_ESP32 Beta-1 March 2026 - CoCo 2 Emulator for ESP32-S3
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32_CoCo2_XRoar_Port
 *   Based on XRoar by Ciaran Anscomb
 *   ESP32 Port of XRoar co-developed with Claude Code (Anthropic)
 *   MIT License
 * ============================================================
 *  File   : hal_audio.cpp
 *  Module : Audio HAL — LEDC PWM pseudo-DAC with cycle-accurate playback
 * ============================================================
 */

/*
 * hal_audio.cpp - CoCo audio output via LEDC PWM on ESP32-S3
 *
 * ESP32-S3 has NO internal DAC — uses LEDC PWM at 78 kHz with
 * 8-bit resolution for pseudo-analog audio output.
 *
 * Pitch-corrected playback:
 *   The emulated CPU runs ~2.6x faster than a real 0.895 MHz 6809,
 *   so DAC transitions happen too fast in wall time. To fix pitch
 *   without slowing the CPU, we buffer audio at scanline rate
 *   (262 samples/frame) and play back at the correct rate via the
 *   22050 Hz ISR. The ISR loops the buffer during render gaps,
 *   which is seamless for periodic tones (SOUND command).
 *
 * Two audio paths (matching real CoCo hardware):
 *   1. Single-bit: PIA1 port B bit 1 (cassette, simple beeps)
 *   2. 6-bit DAC:  PIA1 port A bits 2-7 (SOUND command, music)
 *
 * Based on CoCo_Audio_Test/CoCoAudio implementation.
 */

#include "hal.h"
#include "../utils/debug.h"
#include "soc/ledc_struct.h"


// Audio output pin (LEDC PWM)
#define AUDIO_DAC_PIN       17
#define AUDIO_LEDC_FREQ     78125    // 78.125 kHz PWM
#define AUDIO_LEDC_BITS     8        // 8-bit resolution (0-255)
#define AUDIO_ISR_RATE      22050    // Sample rate

// ---- Pitch-corrected scanline buffer ----
// 262 samples per frame, double-buffered. ISR plays back at correct rate.
#define SCANLINES_PER_FRAME  262
#define SAMPLES_PER_FRAME    SCANLINES_PER_FRAME

// ISR stride: how many scanline-samples to advance per ISR tick (Q8 fixed-point)
// Base value: 262 * 256 * 60 / 22050 ≈ 183
// AUDIO_PITCH_TRIM: fine-tune adjustment (each unit ≈ 0.55% pitch change)
//   Negative = lower pitch, Positive = higher pitch
//   -4 compensates ~2.2% overshoot from scanline quantization
#define AUDIO_PITCH_TRIM  -6
#define ISR_STRIDE_Q8  (((SAMPLES_PER_FRAME * 256 * 60 + AUDIO_ISR_RATE/2) / AUDIO_ISR_RATE) + AUDIO_PITCH_TRIM)

static uint8_t audio_scanline_buf[2][SAMPLES_PER_FRAME];  // double buffer
static volatile int  audio_write_buf = 0;   // CPU writes to this one
static volatile int  audio_read_buf  = 1;   // ISR reads from this one
static volatile int  audio_write_pos = 0;   // next scanline to write
static volatile uint32_t audio_isr_pos_q8 = 0;  // ISR read position (Q8 fixed-point)
static volatile bool audio_buf_ready = false;    // new buffer committed

// LEDC channel
static volatile uint8_t audio_ledc_channel = 0;

// Current audio output level (0-255), set by either audio path
// Still used as the "live" value captured at each scanline
static volatile uint8_t audio_current_level = 128;

static hw_timer_t* audio_timer = nullptr;

// ISR — plays back from scanline buffer at correct CoCo rate
static void IRAM_ATTR audio_timer_isr() {
    // Check for new buffer from CPU
    if (audio_buf_ready) {
        // Swap: ISR takes the just-committed buffer
        audio_read_buf = audio_write_buf;
        audio_write_buf = 1 - audio_write_buf;
        audio_isr_pos_q8 = 0;
        audio_buf_ready = false;
    }

    // Read sample from playback buffer
    const uint8_t* buf = audio_scanline_buf[audio_read_buf];
    uint32_t idx = audio_isr_pos_q8 >> 8;

    // Wrap for looping (seamless for periodic tones)
    if (idx >= SAMPLES_PER_FRAME) {
        idx %= SAMPLES_PER_FRAME;
        audio_isr_pos_q8 = idx << 8;
    }

    uint8_t sample = buf[idx];

    // Advance playback position
    audio_isr_pos_q8 += ISR_STRIDE_Q8;

    // Output to PWM
    LEDC.channel_group[0].channel[audio_ledc_channel].duty.duty = sample << 4;
    LEDC.channel_group[0].channel[audio_ledc_channel].conf0.low_speed_update = 1;
    LEDC.channel_group[0].channel[audio_ledc_channel].conf1.duty_start = 1;
}

void hal_audio_init(void) {
    // Initialize LEDC PWM
    audio_ledc_channel = 0;
    ledcAttachChannel(AUDIO_DAC_PIN, AUDIO_LEDC_FREQ, AUDIO_LEDC_BITS, audio_ledc_channel);
    ledcWrite(AUDIO_DAC_PIN, 128);

    // Fill both buffers with silence (midpoint)
    memset(audio_scanline_buf[0], 128, SAMPLES_PER_FRAME);
    memset(audio_scanline_buf[1], 128, SAMPLES_PER_FRAME);

    // Initialize timer ISR at sample rate
    audio_timer = timerBegin(1000000);
    timerAttachInterrupt(audio_timer, audio_timer_isr);
    timerAlarm(audio_timer, 1000000 / AUDIO_ISR_RATE, true, 0);

    audio_current_level = 128;

    DEBUG_PRINTF("  Audio: LEDC PWM on GPIO%d, %d Hz ISR, stride Q8=%d",
                 AUDIO_DAC_PIN, AUDIO_ISR_RATE, ISR_STRIDE_Q8);
}

void hal_audio_write_sample(int16_t left, int16_t right) {
    (void)left;
    (void)right;
}

void hal_audio_set_volume(uint8_t volume) {
    DEBUG_PRINTF("  Audio: volume set to %d", volume);
}

// Called once per scanline from machine_run_frame to capture audio level
void hal_audio_capture_scanline(void) {
    int pos = audio_write_pos;
    if (pos < SAMPLES_PER_FRAME) {
        audio_scanline_buf[audio_write_buf][pos] = audio_current_level;
        audio_write_pos = pos + 1;
    }
}

// Called at frame end from machine_run_frame to commit the buffer
void hal_audio_commit_frame(void) {
    // Pad remaining scanlines with last level (shouldn't happen normally)
    uint8_t last = (audio_write_pos > 0)
        ? audio_scanline_buf[audio_write_buf][audio_write_pos - 1]
        : 128;
    while (audio_write_pos < SAMPLES_PER_FRAME) {
        audio_scanline_buf[audio_write_buf][audio_write_pos++] = last;
    }

    // Signal ISR to swap buffers
    audio_buf_ready = true;
    audio_write_pos = 0;
}

// Single-bit audio: PIA1 port B bit 1
void hal_audio_write_bit(bool value) {
    audio_current_level = value ? 255 : 0;
}

// Called from main loop once per frame (reserved for future diagnostics)
void hal_audio_debug_tick(void) {
}

// 6-bit DAC audio: PIA1 port A bits 2-7 (value 0-63)
void hal_audio_write_dac(uint8_t dac6) {
    // Scale 6-bit (0-63) to 8-bit (0-255)
    audio_current_level = (dac6 << 2) | (dac6 >> 4);
}
