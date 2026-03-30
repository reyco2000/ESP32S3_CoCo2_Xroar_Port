# ESP32_CoCo2_XRoar_Port Audio HAL — Implementation Details

## Overview

The audio subsystem emulates the CoCo's two audio output paths using LEDC PWM on the ESP32-S3 (which has no internal DAC). A timer ISR at 22,050 Hz pushes the current sample level to the PWM hardware.

**Source files:**
- `src/hal/hal_audio.cpp` — LEDC PWM init, ISR, DAC and single-bit write functions
- `src/hal/hal.h` — Public API declarations
- `src/core/machine.cpp` — Sound MUX gating logic (PIA1/PIA0 wiring)
- `config.h` — Pin and sample rate configuration

---

## Hardware Being Emulated

### Real CoCo Audio Architecture

```
  PIA1 Port A bits 2-7           PIA1 Port B bit 1
  (6-bit DAC value)              (single-bit audio)
        │                               │
        v                               │
   ┌──────────┐                         │
   │ 6-bit    │                         │
   │ R-2R DAC │                         │
   └────┬─────┘                         │
        │                               │
        v                               v
   ┌─────────────────────────────────────────┐
   │         Analog MUX (4066 / MC14066)     │
   │                                         │
   │  SEL1 (PIA0 CA2)  SEL2 (PIA0 CB2)      │
   │       │                  │              │
   │  Source select:                         │
   │    00 = 6-bit DAC                       │
   │    01 = Cassette input                  │
   │    10 = Cartridge audio                 │
   │    11 = No source                       │
   │                                         │
   │  MUX Enable: PIA1 CRB bit 3            │
   │    1 = Route selected source to speaker │
   │    0 = Disconnect (mute DAC)            │
   └────────────────┬────────────────────────┘
                    │
                    v
                 Speaker
```

### Register Map

| Register | Address | Bits | Function |
|----------|---------|------|----------|
| PIA1 DA | $FF20 | 2-7 | 6-bit DAC value (0-63) |
| PIA1 DB | $FF22 | 1 | Single-bit audio toggle |
| PIA1 CRB | $FF23 | 3 | Sound MUX enable (1=on, 0=mute) |
| PIA0 CRA | $FF01 | 3 | MUX source select bit 0 (CA2 output) |
| PIA0 CRB | $FF03 | 3 | MUX source select bit 1 (CB2 output) |

### MUX Source Selection

| SEL2 (CB2) | SEL1 (CA2) | Source | Used by |
|:---:|:---:|--------|---------|
| 0 | 0 | **6-bit DAC** | SOUND, PLAY, game audio |
| 0 | 1 | Cassette input | Not emulated |
| 1 | 0 | Cartridge audio | Not emulated |
| 1 | 1 | None (silence) | — |

---

## ESP32-S3 Implementation

### Hardware Configuration

| Parameter | Value | Notes |
|-----------|-------|-------|
| Output pin | GPIO17 | Configured in `config.h` as `PIN_DAC_OUT` |
| PWM frequency | 78,125 Hz | High enough to be inaudible; set by `AUDIO_LEDC_FREQ` |
| PWM resolution | 8-bit (0-255) | LEDC channel 0 |
| Sample rate | 22,050 Hz | Timer ISR fires at this rate |
| Idle level | 128 (midpoint) | Silence = half duty cycle |

### Audio Paths

**Two independent paths write to a single shared variable (`audio_current_level`):**

1. **6-bit DAC** (`hal_audio_write_dac`):
   - Input: 6-bit value (0-63) from PIA1 PA bits 2-7
   - Scaling: `(dac6 << 2) | (dac6 >> 4)` maps 0-63 to 0-255 with smooth ramp
   - Used by: SOUND command, PLAY command, game sound effects

2. **Single-bit audio** (`hal_audio_write_bit`):
   - Input: boolean from PIA1 PB bit 1
   - Output: 255 (high) or 0 (low) — full swing square wave
   - Used by: cassette relay toggle, simple beeps (`CHR$(7)`)

**Last write wins** — both paths write to the same `audio_current_level` byte. The real CoCo has the same behavior (single speaker output).

### Pitch-Corrected Scanline Buffer (2026-03-28)

#### The Problem

The emulated 6809 CPU executes ~2.6x faster than a real 0.895 MHz CoCo in wall-clock time (14916 cycles complete in ~6.5ms instead of 16.67ms). The remaining frame time is spent on SPI display rendering. This means DAC transitions from the SOUND command occur too quickly, producing audio roughly 2.6x higher in pitch than correct.

#### The Solution

Instead of writing DAC values directly to the PWM, audio is buffered at scanline rate and played back at the correct CoCo timing:

1. **Capture**: After each scanline in `machine_run_frame()`, `hal_audio_capture_scanline()` records the current `audio_current_level` into a 262-entry double buffer.
2. **Commit**: At frame end, `hal_audio_commit_frame()` signals the ISR to swap to the new buffer.
3. **Playback**: The 22050 Hz ISR walks through the buffer using a Q8 fixed-point stride (`ISR_STRIDE_Q8`) calibrated so 262 samples play back over exactly one CoCo frame period (~16.67ms).
4. **Looping**: When the ISR exhausts the buffer (during the ~20ms render gap between frames), it wraps to the beginning. This is seamless for periodic tones like SOUND.

```
machine_run_frame():
  for each scanline (0-261):
    machine_run_scanline()      → CPU executes ~57 cycles, DAC may change
    hal_audio_capture_scanline() → snapshot audio_current_level into buffer
  hal_audio_commit_frame()       → signal ISR to swap to new buffer

audio_timer_isr() [IRAM_ATTR, 22050 Hz]:
  1. If new buffer ready: swap read/write buffers, reset position
  2. Read sample from playback buffer at Q8 index
  3. Advance index by ISR_STRIDE_Q8 (wraps at 262 for looping)
  4. Write to LEDC duty register
```

#### Pitch Fine-Tuning

The base stride is calculated as `262 * 256 * 60 / 22050 ≈ 183`. An `AUDIO_PITCH_TRIM` constant (currently -6) adjusts for residual error from scanline-boundary quantization. Each trim unit changes pitch by ~0.55%.

| Trim | Stride | Effect |
|------|--------|--------|
| 0 | 183 | Base rate (slightly sharp) |
| -6 | 177 | Current setting (~3.3% lower) |

**Measured accuracy**: With trim=-6, `SOUND 84,20` on ESP32 matches `SOUND 82,20` on XRoar (desktop emulator) — within ~1% of correct pitch. Residual error is attributed to scanline-boundary quantization of DAC transitions.

#### Bandwidth

Scanline-rate sampling gives an effective sample rate of 262 × 60 = 15,720 Hz (Nyquist limit ~7.8 kHz). This covers the full CoCo audio range — the SOUND command's maximum frequency is well under 4 kHz.

### Timer ISR

The ISR uses direct register access (`LEDC.channel_group[0].channel[N]`) for minimum latency (~1 us per invocation). The `IRAM_ATTR` ensures it stays in fast internal RAM.

---

## Sound MUX Gating (Bug Fix — 2026-03-26)

### The Problem

Running `10 PRINT JOYSTK(0) : 20 GOTO 10` produced unwanted speaker noise. The BASIC `JOYSTK()` routine performs successive approximation ADC by rapidly writing ~32 different DAC values to PIA1 PA during each joystick read. Each write was unconditionally forwarded to `hal_audio_write_dac()`, producing audible clicks at the joystick polling rate.

On real CoCo hardware, the BASIC ROM routine (`GETJOY`) clears PIA1 CRB bit 3 before the ADC loop, which disconnects the DAC from the speaker via the analog MUX. After reading, it restores the bit.

### The Fix

DAC audio writes in `machine.cpp` are now gated by two conditions:

1. **MUX enabled**: PIA1 CRB bit 3 must be set
2. **MUX source = DAC**: PIA0 CRA bit 3 and CRB bit 3 must both be 0

```
machine_write() PIA1 handler:
  On PIA1 DA or CRA write:
    mux_en  = (pia1.ctrl_b & 0x08) != 0
    mux_src = ((pia0.ctrl_b & 0x08) >> 2) | ((pia0.ctrl_a & 0x08) >> 3)
    if (mux_en AND mux_src == 0):
      hal_audio_write_dac(...)  // Output DAC value
    // else: silently ignore (DAC disconnected from speaker)

  On PIA1 CRB write (MUX enable may have changed):
    if (mux just re-enabled AND source == DAC):
      hal_audio_write_dac(current PA value)  // Restore DAC audio immediately
```

### Interaction Table

| Operation | PIA1 CRB bit 3 | MUX Source | DAC audible? | Single-bit audible? |
|-----------|:---:|:---:|:---:|:---:|
| `SOUND 200,5` | 1 | 0 (DAC) | Yes | Independent |
| `PLAY "CDEFG"` | 1 | 0 (DAC) | Yes | Independent |
| `JOYSTK(0)` ADC loop | 0 | — | **No** | Independent |
| `PRINT CHR$(7)` beep | — | — | — | **Yes** |
| Game audio + joystick | Alternates | 0 (DAC) | During sound only | Independent |

### Key Insight: Shared DAC Resource

The 6-bit DAC (PIA1 PA bits 2-7) serves **dual purposes** simultaneously:
- **Audio output**: DAC value feeds the R-2R ladder to produce an analog voltage for the speaker
- **Joystick threshold**: Same DAC value is compared against the joystick potentiometer voltage

The MUX doesn't affect the joystick comparator — it only controls whether the DAC voltage reaches the **speaker**. The comparator reads the PIA register directly, not the analog output. This means our emulation can (and does) handle both uses from the same PIA register without conflict.

---

## Lessons Learned

1. **Shared PIA resources are a CoCo design signature.** The CoCo reuses PIAs extensively for cost savings. The same bits that produce audio also read joysticks. The same MUX select lines that choose audio source also select joystick port/axis. Always check the full hardware context before assuming a PIA bit has a single purpose.

2. **XRoar's `sound.c` is the reference** for correct MUX behavior. It implements full source selection with gain tables matching real hardware voltage levels. Our simplified version (gate on/off only) is sufficient because we don't emulate cassette or cartridge audio.

3. **Single-bit audio (PIA1 PB1) is independent of the MUX.** It bypasses the MUX entirely on real hardware and should never be gated by the MUX enable bit.

4. **The ISR approach (22 kHz timer → PWM) works well on ESP32-S3.** The LEDC PWM at 78 kHz produces a pseudo-analog signal that sounds acceptable through a small speaker. The timer ISR with `IRAM_ATTR` adds minimal overhead (~22,050 calls/sec at ~1 us each = ~2.2% CPU).

5. **No external DAC needed.** The ESP32-S3 lacks an internal DAC (unlike ESP32), but LEDC PWM with an RC filter on the output pin produces adequate audio quality for 6-bit CoCo sound.

6. **Pitch correction requires buffered playback, not CPU throttling.** The CPU runs faster than real-time during each frame burst, making audio timing-dependent code (SOUND, PLAY) produce the wrong pitch. Scanline-rate buffering with ISR playback at the correct CoCo rate fixes pitch without sacrificing emulation speed (~25-27 fps). The looping buffer handles the render gap seamlessly for periodic tones. Residual ~1% pitch error comes from scanline-boundary quantization of DAC transitions.
