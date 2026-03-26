# ESP32_CoCo2_XRoar_Port Joystick HAL — Implementation Details

## Overview

The joystick subsystem emulates two CoCo analog joystick ports. Each port provides two 6-bit analog axes (X/Y, range 0-63) and one fire button. The CoCo uses a software successive approximation ADC with the PIA's 6-bit DAC as comparator threshold — there is no dedicated ADC chip.

**Source files:**
- `src/hal/hal_joystick.cpp` — HAL interface (axis read, compare, update)
- `src/hal/CoCoJoystick.h` — Joystick driver class declaration
- `src/hal/CoCoJoystick.cpp` — ADC reading, filtering, calibration, comparator
- `src/core/machine.cpp` — PIA0 PA7 comparator wiring (lines 123-148)
- `config.h` — GPIO pin assignments

---

## Hardware Being Emulated

### Real CoCo Joystick Circuit

```
   Joystick pot (0-5V)          PIA1 PA bits 2-7
          │                     (6-bit DAC value)
          │                            │
          v                            v
     ┌─────────┐              ┌──────────────┐
     │ Analog  │              │  6-bit       │
     │ MUX     │              │  R-2R DAC    │
     │         │              │  (0-5V out)  │
     │ SEL1/2  │              └──────┬───────┘
     │ (CA2/   │                     │
     │  CB2)   │                     │
     └────┬────┘                     │
          │                          │
          v                          v
     ┌────────────────────────────────────┐
     │        Analog Comparator           │
     │  Output: 1 if joy >= dac           │
     │          0 if joy <  dac           │
     └──────────────┬─────────────────────┘
                    │
                    v
              PIA0 PA bit 7
```

### MUX Select Lines (Axis/Port Selection)

| SEL2 (PIA0 CB2) | SEL1 (PIA0 CA2) | Joystick Input |
|:---:|:---:|------|
| 0 | 0 | Right joystick, X axis |
| 0 | 1 | Right joystick, Y axis |
| 1 | 0 | Left joystick, X axis |
| 1 | 1 | Left joystick, Y axis |

**Note:** These are the **same MUX select lines** used for audio source selection. The CoCo reuses them for both purposes (see `audio-hal.md`).

### BASIC ROM ADC Routine (GETJOY)

The CoCo has no hardware ADC. BASIC performs successive approximation in software:

```
GETJOY:
  1. Disable sound MUX (clear PIA1 CRB bit 3) — mutes DAC speaker output
  2. Set CA2/CB2 for desired port/axis
  3. For bit = 5 downto 0:
     a. Write trial DAC value to PIA1 PA
     b. Read PIA0 PA bit 7 (comparator result)
     c. If joy >= dac: keep bit set; else: clear bit
  4. Re-enable sound MUX (set PIA1 CRB bit 3)
  5. Return 6-bit result (0-63)
```

This takes ~32 DAC writes per axis read. Without MUX gating, each write would produce audible clicks (see `audio-hal.md` for the fix).

### Fire Buttons

Fire buttons are wired in parallel with keyboard matrix rows on PIA0 PA:
- **Right joystick button** → PIA0 PA bit 0 (shared with keyboard row 0)
- **Left joystick button** → PIA0 PA bit 1 (shared with keyboard row 1)

When pressed, the bit reads as 0 (active low), same as a keyboard key press.

---

## ESP32-S3 Implementation

### GPIO Pin Assignments

| Signal | GPIO | ADC Channel | Notes |
|--------|------|-------------|-------|
| Joy 0 X | 9 | ADC1_CH8 | Right joystick horizontal |
| Joy 0 Y | 8 | ADC1_CH7 | Right joystick vertical |
| Joy 0 Button | 18 | — | Digital input, internal pullup |
| Joy 1 X | 16 | ADC2_CH5 | Left joystick horizontal |
| Joy 1 Y | 15 | ADC2_CH4 | Left joystick vertical |
| Joy 1 Button | 7 | — | Digital input, internal pullup |

### CoCoJoystick Driver

The `CoCoJoystick` class handles analog reading, filtering, and scaling:

**ADC Reading Pipeline:**
```
analogReadMilliVolts(pin)
  → Moving average (4 samples)
  → Dead zone filter (±50 mV around center)
  → Scale to 0-63 range using calibration min/max
```

**Key parameters:**
| Parameter | Value | Purpose |
|-----------|-------|---------|
| `AVG_SAMPLES` | 4 | Moving average window for noise reduction |
| `DEAD_ZONE_MV` | 50 | Millivolt dead zone around center position |
| `BTN_DEBOUNCE_MS` | 20 | Button debounce time in milliseconds |

### ADC Update Throttling

Joystick ADC is NOT read every time PIA0 PA is accessed (which can be thousands of times per frame in a tight polling loop). Instead, `hal_joystick_update()` is called once per frame from `hal_process_input()`, and additional updates happen every 16 scanlines during PIA0 reads:

```cpp
// In machine_read(), PIA0 PA handler:
static uint32_t last_joy_scanline = UINT32_MAX;
uint32_t joy_slot = m->scanline >> 4;  // divide by 16
if (joy_slot != last_joy_scanline) {
    last_joy_scanline = joy_slot;
    hal_joystick_update();  // Refresh ADC
}
```

This gives ~16 ADC updates per frame — responsive enough for games without excessive overhead.

### Comparator Emulation

The CoCo's analog comparator is emulated digitally in `machine_read()`:

```cpp
// PIA0 PA read — joystick comparator on bit 7:
int joy_port = (pia0.ctrl_b & 0x08) >> 3;     // CB2 = port select
int joy_axis = (pia0.ctrl_a & 0x08) >> 3;     // CA2 = axis select
int dac_value = ((pia1.data_a & pia1.ddr_a) & 0xFC) + 2;  // 8-bit range
int js_value = hal_joystick_read_axis(port, axis) * 4 + 2; // Scale to match
if (js_value >= dac_value)
    row_data |= 0x80;   // PA7 = 1 (joy >= threshold)
else
    row_data &= ~0x80;  // PA7 = 0 (joy < threshold)
```

**Scaling detail:** Both values are converted to 8-bit range (2-254) for comparison:
- DAC: `(PIA1 PA & 0xFC) + 2` — mask low 2 bits, add offset
- Joystick: `axis_6bit * 4 + 2` — scale 0-63 to 2-254

This matches XRoar's `joystick_update()` exactly.

### Calibration

The `CoCoJoystick` class supports runtime calibration:
- `calibrate_begin()` — Start calibration (record current position as center)
- `calibrate_update()` — Track min/max during stick movement
- `calibrate_end()` — Finalize calibration ranges
- `save_calibration()` / `load_calibration()` — Persist to NVS

Default calibration assumes 0-3300 mV range with 1650 mV center (3.3V ESP32 ADC reference).

---

## Data Flow Summary

```
ESP32 ADC pin
  │
  v
analogReadMilliVolts()
  │
  v
CoCoJoystick::update() ─── moving average ─── dead zone ─── scale to 0-63
  │                                                              │
  │                                                              v
  │                                                     CoCoJoystick::get_x/y()
  │                                                              │
  v                                                              v
hal_joystick_update()                              hal_joystick_read_axis()
(called 16x/frame)                                 (called on PIA0 PA read)
                                                             │
                                                             v
                                          machine_read() comparator logic
                                            dac_value vs js_value
                                                             │
                                                             v
                                                      PIA0 PA bit 7
                                                      (to CPU via PIA read)
```

---

## Lessons Learned

1. **The DAC is shared between audio and joystick.** PIA1 PA bits 2-7 serve as both the audio DAC output and the joystick comparator threshold. The sound MUX (PIA1 CRB bit 3) controls whether DAC writes reach the speaker, but the comparator always reads the PIA register directly regardless of MUX state. See `audio-hal.md` for the MUX gating fix that prevents JOYSTK() noise.

2. **Fire buttons share keyboard matrix lines.** Right button = PA0, Left button = PA1. These are the same bits as keyboard rows 0 and 1. Code must OR the button state into the keyboard scan result, not replace it. Current implementation handles this in `machine_read()` PIA0 handler.

3. **ADC update throttling is essential.** CoCo programs (especially assembly games) may read PIA0 PA hundreds of times per frame in tight polling loops. Calling `analogReadMilliVolts()` on every read would devastate performance. The 16-scanline throttle provides responsive input (~16 updates/frame ≈ ~960 Hz effective sample rate) with negligible overhead.

4. **The successive approximation ADC is a software algorithm, not hardware.** The CoCo CPU performs the binary search by writing DAC values and reading the comparator. Our emulation doesn't need to replicate the algorithm — we just need the comparator to return the correct result for any given DAC threshold. The BASIC ROM (or game code) does the rest.

5. **MUX select lines (CA2/CB2) are shared with audio source selection.** The same 2-bit field selects both the joystick port/axis AND the audio source. Software must manage these carefully — BASIC's GETJOY routine sets them for joystick reading and restores them for audio afterward.

6. **Calibration matters for real potentiometers.** CoCo joysticks use 100K linear pots with varying resistance ranges. The moving average and dead zone filter in `CoCoJoystick` compensate for noisy ADC readings. Without filtering, the joystick value jitters by ±2-3 units, which is visible in BASIC (`PRINT JOYSTK(0)` shows unstable values).
