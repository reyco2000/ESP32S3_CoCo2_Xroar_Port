# ESP32_CoCo2_XRoar_Port Core Modules — Implementation Details

## Overview

The `ESP32_CoCo2_XRoar_Port/src/core/` directory contains the emulation core: the four major ICs of the CoCo 2 plus the machine integration layer that wires t
hem together. All modules are derived from XRoar's C source by Ciaran Anscomb, adapted to C++ for the ESP32-S3 Arduino environment.

**Source files:**

| File | IC / Role | Lines |
|------|-----------|-------|
| `mc6809.h` / `mc6809.cpp` | Motorola MC6809 CPU | ~2,650 |
| `mc6809_opcodes.h` | Opcode definitions & cycle tables | 88 |
| `mc6821.h` / `mc6821.cpp` | Motorola 6821 PIA (×2) | 175 |
| `mc6847.h` / `mc6847.cpp` | Motorola MC6847 VDG | 396 |
| `sam6883.h` / `sam6883.cpp` | SAM6883 Address Multiplexer | 164 |
| `machine.h` / `machine.cpp` | System integration & memory map | 583 |

---

## MC6809 — CPU (`mc6809.h`, `mc6809.cpp`)

Full emulation of the Motorola MC6809E 8-bit processor, the heart of the CoCo.

### Registers

| Register | Width | Description |
|----------|-------|-------------|
| `pc` | 16-bit | Program counter |
| `d` | 16-bit | Accumulator D (A = high byte, B = low byte) |
| `x`, `y` | 16-bit | Index registers |
| `s` | 16-bit | Hardware stack pointer |
| `u` | 16-bit | User stack pointer |
| `dp` | 8-bit | Direct page register |
| `cc` | 8-bit | Condition codes (E, F, H, I, N, Z, V, C) |

### Condition Code Flags

| Flag | Bit | Description |
|------|-----|-------------|
| E | 7 | Entire state saved (1 = full push on interrupt, 0 = partial/FIRQ) |
| F | 6 | FIRQ mask (1 = FIRQ disabled) |
| H | 5 | Half carry (bit 3 carry, used by DAA) |
| I | 4 | IRQ mask (1 = IRQ disabled) |
| N | 3 | Negative (MSB of result) |
| Z | 2 | Zero (result == 0) |
| V | 1 | Overflow (signed arithmetic) |
| C | 0 | Carry / borrow |

### Execution Loop (`mc6809_run`)

```
mc6809_run(cpu, budget):
  cycles = 0
  while cycles < budget:
    if cpu->halted:
      cycles = budget; break          // FDC HALT burns budget
    check_interrupts(cpu)             // NMI > FIRQ > IRQ priority
    if cpu->wait_for_interrupt:
      cycles = budget; break          // CWAI/SYNC idles
    execute_one(cpu)                  // Fetch-decode-execute
  return cycles
```

- **HALT support**: When `cpu->halted` is true (set by the FDC's DRQ/HALT mechanism), the CPU burns its entire budget doing nothing — essential fo
r disk I/O synchronization.
- **CWAI/SYNC**: `wait_for_interrupt` flag causes the CPU to idle until an interrupt arrives.

### Interrupt Handling

Three interrupt types, checked before each instruction in priority order:

**NMI** (`mc6809_nmi(cpu, active)`)
- **Edge-triggered**: Latches `nmi_pending` on inactive→active transition of `nmi_line`. After servicing, `nmi_line` is cleared — a new edge is re
quired for the next NMI.
- **Non-maskable**: Cannot be disabled via CC flags.
- **nmi_armed gate**: NMI is ignored until the first LDS instruction executes (per MC6809 spec). This prevents spurious NMI during reset when S is
 uninitialized.
- **Stack push**: Full state (E=1) — CC, A, B, DP, X, Y, U, PC → 12 bytes on S stack (19 cycles). If `cwai_state` is true, push is skipped (7 cycl
es).
- **Masks**: Sets both I and F flags.
- **Vector**: $FFFC.
- **Used by**: FDC INTRQ for disk transfer completion.

**FIRQ** (`mc6809_firq(cpu, active)`)
- **Level-triggered**: `firq_pending` mirrors the pin state.
- **Masked by**: F flag in CC.
- **Fast**: Pushes only CC and PC (E=0) — 3 bytes (10 cycles). Exception: if `cwai_state` is true, the full state was already pushed with E=1 by C
WAI.
- **Masks**: Sets both I and F flags.
- **Vector**: $FFF6.
- **Used by**: Cartridge interrupt (PIA1 IRQA/IRQB).

**IRQ** (`mc6809_irq(cpu, active)`)
- **Level-triggered**: `irq_pending` mirrors the pin state.
- **Masked by**: I flag in CC.
- **Stack push**: Full state (E=1) — 12 bytes (19 cycles), or 7 cycles if CWAI.
- **Masks**: Sets I flag only (F unchanged).
- **Vector**: $FFF8.
- **Used by**: 60Hz vsync timer (PIA0 CB1) and keyboard.

**CWAI and SYNC operations:**
- **CWAI** ($3C): ANDs an immediate byte with CC (clearing mask bits to allow interrupt), sets E=1, pre-pushes entire state to S stack, then enter
s `wait_for_interrupt`. When an interrupt arrives, the handler skips the redundant push — only vectoring and masking are needed (7 cycles instead
of 19).
- **SYNC** ($13): Enters `wait_for_interrupt` without pushing state. The interrupt that wakes SYNC causes a normal push-and-vector sequence.

### Interrupt Vector Table

| Vector | Address | Use |
|--------|---------|-----|
| RESET | $FFFE | Power-on / reset |
| NMI | $FFFC | FDC disk transfer |
| SWI | $FFFA | Software interrupt |
| IRQ | $FFF8 | 60Hz timer, keyboard |
| FIRQ | $FFF6 | Cartridge |
| SWI2 | $FFF4 | (unused on CoCo) |
| SWI3 | $FFF2 | (unused on CoCo) |

### Opcode Coverage

All documented MC6809 opcodes are implemented across three pages:

- **Page 1** (no prefix): 8-bit ALU (ADD, ADC, SUB, SBC, AND, OR, EOR, CMP, TST, NEG, COM, CLR, INC, DEC, LSR, LSL/ASL, ASR, ROR, ROL), loads/stor
es (LD, ST for A, B, D, X, Y, S, U), branches (BRA, BEQ, BNE, BCC, BCS, BPL, BMI, BVS, BVC, BGE, BGT, BLE, BLT, BHI, BLS, BSR), stack ops (PSHS, P
ULS, PSHU, PULU), LEA (LEAX, LEAY, LEAS, LEAU), TFR, EXG, MUL, DAA, SEX, ABX, NOP, SYNC, CWAI, SWI, RTI, RTS
- **Page 2** (prefix `$10`): 16-bit comparisons (CMPD, CMPY), long branches (LBRA, LBSR, LBcc), LDY/STY, LDS/STS, SWI2
- **Page 3** (prefix `$11`): CMPU, CMPS, SWI3

### Addressing Modes

All MC6809 addressing modes are implemented:

| Mode | Syntax | Example | Notes |
|------|--------|---------|-------|
| Inherent | — | CLRA | No operand |
| Immediate 8 | #nn | LDA #$42 | |
| Immediate 16 | #nnnn | LDD #$1234 | |
| Direct | dp:nn | LDA $30 | DP register provides high byte |
| Extended | nnnn | LDA $1234 | Full 16-bit address |
| Indexed | various | LDA ,X | Complex postbyte decoding (see below) |
| Relative 8 | offset | BNE loop | Signed 8-bit (-128..+127) |
| Relative 16 | offset | LBNE loop | Signed 16-bit |

**Indexed sub-modes** (decoded from postbyte):
- Constant offset: 5-bit signed, 8-bit signed, 16-bit signed
- Register offset: A,R / B,R / D,R
- Auto-increment: ,R+ / ,R++ (post-increment by 1 or 2)
- Auto-decrement: ,-R / ,--R (pre-decrement by 1 or 2)
- Zero offset: ,R
- PC-relative: 8-bit or 16-bit offset from PC
- Indirect: [any of the above] — adds an extra memory read for the effective address
- Extended indirect: [nnnn]

### `mc6809_opcodes.h`

Reference tables for cycle counts (PROGMEM-ready) and opcode constant definitions. The cycle counts in this header are informational — actual coun
ting is done inline within `mc6809.cpp` to avoid lookup overhead. Also defines common opcode constants (`MC6809_OP_LDA_IMM`, `MC6809_OP_SWI`, etc.
) and TFR/EXG register codes.

### Performance Optimizations

- **Branchless flag computation**: ALU helpers (`op_add8`, `op_sub8`, `op_add16`, `op_sub16`, `update_nz8`, `update_nz16`) use a compute-and-mask
pattern — flags are accumulated into a local variable `f` and written to `cpu->cc` in a single masked OR. The `CC_PUT` macro uses a branchless ter
nary that compiles to Xtensa MOVNEZ. This optimization improved performance from ~23.5 to ~25–27 fps.
- **Inline memory helpers**: `mem_read`, `mem_write`, `fetch8`, `fetch16`, push/pull helpers are all `static inline` to eliminate function call ov
erhead in the hot instruction loop.
- **D register as single uint16_t**: A and B are stored as the high and low bytes of a single `uint16_t d`, accessed via `GET_A()` / `GET_B()` mac
ros and `SET_A()` / `SET_B()`. This makes 16-bit D operations (ADDD, SUBD, LDD, STD) naturally efficient.
- **No IRAM_ATTR**: Testing showed that placing CPU functions in IRAM actually hurt performance on ESP32-S3 (flash cache is faster than IRAM for l
arge code).

---

## MC6821 — PIA (`mc6821.h`, `mc6821.cpp`)

Emulates the Motorola 6821 Peripheral Interface Adapter. Two instances are used in the CoCo:

| Instance | Address | Role |
|----------|---------|------|
| **PIA0** | $FF00–$FF03 | Keyboard matrix, joystick comparator, hsync (CA1), vsync (CB1) |
| **PIA1** | $FF20–$FF23 | 6-bit DAC audio (PA2–PA7), single-bit audio (PB1), VDG mode (PB3=CSS, PB7=AG), cartridge IRQ |

### Registers (per port, ×2 ports per PIA)

| Register | Offset | Description |
|----------|--------|-------------|
| Data/DDR A | 0 | Port A data or direction register (selected by CRA bit 2) |
| Control A | 1 | IRQ flags (bits 7–6, read-only), CA2 control, DDR select, CA1 edge/enable |
| Data/DDR B | 2 | Port B data or direction register (selected by CRB bit 2) |
| Control B | 3 | IRQ flags, CB2 control, DDR select, CB1 edge/enable |

### Control Register Bit Layout

```
Bit 7: IRQ1 flag (read-only) — set by CA1/CB1 transition matching edge select
Bit 6: IRQ2 flag (read-only) — set by CA2/CB2 transition (if configured as input)
Bit 5: CA2/CB2 direction (1 = output, 0 = input)
Bit 4: CA2/CB2 output control (when bit 5 = 1)
Bit 3: CA2/CB2 control / edge select (when bit 5 = 0)
Bit 2: DDR/Data select (0 = DDR, 1 = data register)
Bit 1: CA1/CB1 edge select (0 = falling, 1 = rising)
Bit 0: CA1/CB1 IRQ enable (1 = enable IRQ output)
```

### Key Operation Details

**DDR/Data selection:**
- Control register bit 2 selects whether offset 0/2 accesses the Data Direction Register (DDR) or the data register.
- DDR bit = 0 means the corresponding pin is an input; DDR bit = 1 means output.
- After reset, all DDR bits are 0 (all inputs) and CRx bit 2 is 0 (DDR selected). BASIC ROM configures the DDR first, then sets bit 2 to switch to
 data mode.

**Read behavior:**
- Port A reads: output bits come from `data_a & ddr_a`, input bits from `input_a & ~ddr_a` (mixed read).
- Port B reads: identical mixing of output and input bits.
- Reading the data register clears both IRQ flags (bits 7 and 6 of the control register) and recalculates the IRQ output — this is how BASIC ackno
wledges the 60Hz timer.

**Write behavior:**
- Writing the control register preserves bits 7–6 (read-only IRQ flags); only bits 5–0 are written.
- Writing data register stores the value; only bits with DDR=1 appear on the output pins.

**CA1/CB1 edge-triggered interrupts:**
- The `mc6821_ca1_transition()` / `mc6821_cb1_transition()` functions accept a `bool rising` parameter.
- Edge polarity is configurable via control register bit 1: 0 = falling edge, 1 = rising edge.
- When the transition matches the configured edge, IRQ1 flag (bit 7) is set.
- The IRQ output is asserted if `(IRQ1 flag && IRQ1 enable)` OR `(IRQ2 flag && IRQ2 enable && CA2/CB2 is input)`.

**IRQ output callbacks:**
- Each port has an `irq_a_callback` / `irq_b_callback` function pointer.
- The callback fires whenever the computed IRQ output state changes.
- `pia_update_irq_a/b()` is called after any flag set/clear or control register write.

**Reset behavior:**
- All registers zeroed, all pins become inputs (DDR = 0), `input_a/b` set to $FF (pulled high).
- IRQ callbacks are preserved across reset and called with `false` to deassert.

### CoCo-Specific Wiring

```
PIA0 IRQA, IRQB → CPU IRQ    (60Hz vsync timer + keyboard)
PIA1 IRQA, IRQB → CPU FIRQ   (cartridge)
PIA0 CB1 ← VDG FS            (vsync — falling edge triggers 60Hz IRQ)
PIA0 PA0–PA7 ← keyboard rows (active-low, scanned by ROM)
PIA0 PB0–PB7 → keyboard cols (column select)
PIA0 PA7 ← joystick DAC comparator result
PIA0 CRA bit 3 (CA2 output) → joystick axis select (0=X, 1=Y)
PIA0 CRB bit 3 (CB2 output) → joystick port select (0=right, 1=left)
PIA1 PA2–PA7 → 6-bit DAC     (SOUND / PLAY audio)
PIA1 PB1 → single-bit audio  (cassette relay, beep)
PIA1 PB3 → VDG CSS           (color set select)
PIA1 PB7 → VDG AG            (alpha/graphics select)
```

### 60Hz Timer Operation (detailed flow)

```
1. VDG field sync (scanline 192) → FS falling edge
2. mc6821_cb1_transition(&pia0, false)
3. CoCo has ctrl_b bit 1 = 0 → falling edge match
4. Sets IRQ1 flag (ctrl_b bit 7)
5. pia_update_irq_b() → callback fires mc6809_irq(true)
6. CPU services IRQ → reads PIA0 CRB ($FF03) to check flag → reads data B ($FF02)
7. Data register read clears IRQ flags → pia_update_irq_b() → mc6809_irq(false)
8. ROM handler increments TIMVAL ($0112-$0113), processes SOUND/PLAY timing
```

---

## MC6847 — VDG (`mc6847.h`, `mc6847.cpp`)

Emulates the Motorola MC6847 Video Display Generator. Renders 256×192 active pixels into a per-scanline palette-indexed buffer (`line_buffer[256]`
).

### Mode Bits

| Bit | Source | Function |
|-----|--------|----------|
| AG (bit 7) | PIA1 PB7 | 0 = alphanumeric/semigraphics, 1 = graphics |
| CSS (bit 3) | PIA1 PB3 | Color set select (green/orange or alternate palette) |
| GM0–GM2 (bits 0–2) | SAM V0–V2 | Graphics sub-mode (resolution + color depth) |
| INV (bit 4) | VRAM bit 6 | Per-character inverse video |
| AS (bit 6) | VRAM bit 7 | Per-character semigraphics-4 select |

### Color Palette

12 VDG colors mapped to RGB565 values:

| Index | Name | RGB565 | Approximate RGB | Usage |
|-------|------|--------|-----------------|-------|
| 0 | Green | 0x0FE1 | (10,255,10) | Text fg (CSS=0), 2bpp CSS=0 color 0 |
| 1 | Yellow | 0xFFE8 | (255,255,67) | 2bpp CSS=0 color 1 |
| 2 | Blue | 0x20B6 | (34,20,180) | 2bpp CSS=0 color 2, CSS=1 color 1 |
| 3 | Red | 0xB024 | (182,5,34) | 2bpp CSS=0 color 3 |
| 4 | White/Buff | 0xFFFF | (255,255,255) | 2bpp CSS=1 color 3 |
| 5 | Cyan | 0x0EAE | (10,212,112) | Semigraphics |
| 6 | Magenta | 0xF8FF | (255,28,255) | Semigraphics |
| 7 | Orange | 0xFA01 | (255,66,10) | Text fg (CSS=1), 2bpp CSS=1 color 2 |
| 8 | Black | 0x0841 | (9,9,9) | Text bg, 2bpp CSS=1 color 0 |
| 9 | Dark Green | 0x0200 | (0,65,0) | 1bpp CSS=0 background |
| 10 | Dark Orange | 0x6800 | (108,0,0) | 1bpp CSS=1 background |
| 11 | Bright Orange | 0xFDA8 | (255,180,67) | 1bpp CSS=1 foreground |

### Text Mode (AG=0)

- **32×16 characters**, each 8 pixels wide × 12 scanlines tall
- Internal 64-character ROM font (768 bytes, stored in PROGMEM, copied to DRAM at init for fast access)
- Characters 0x00–0x3F: `@`, `A`–`Z`, `[`, `\`, `]`, `↑`, `←`, space, `!`–`?`
- Font data: 6 pixels wide (bits 5–0), centered in 8-pixel cell with 1-pixel padding on each side

**Normal vs Inverse:**
- **Normal** (VRAM bit 6 = 0): bright character on dark background (fg on bg)
- **Inverse** (VRAM bit 6 = 1): dark character on bright background (bg on fg)
- CoCo BASIC stores all visible text as VDG "inverse" ($40–$7F), so the user sees dark-on-green
- CLS fills video memory with $60 (inverse space) — renders as solid green screen

**Semigraphics-4** (VRAM bit 7 = 1):
- 2×2 block per character cell: top-left=bit 3, top-right=bit 2, bottom-left=bit 1, bottom-right=bit 0
- 8 colors from bits 6–4 (VDG color index 0–7)
- Each quadrant is 4 pixels wide × 6 scanlines tall
- Background is always black

### Graphics Modes (AG=1)

| GM | Mode | Resolution | BPP | Bytes/Row | CoCo Name |
|----|------|-----------|-----|-----------|-----------|
| 0 | CG1 | 64×64 | 2 | 16 | — |
| 1 | RG1 | 128×64 | 1 | 16 | — |
| 2 | CG2 | 128×64 | 2 | 32 | PMODE 0 |
| 3 | RG2 | 128×96 | 1 | 16 | PMODE 1 |
| 4 | CG3 | 128×96 | 2 | 32 | PMODE 2 |
| 5 | RG3 | 128×192 | 1 | 16 | PMODE 3 |
| 6 | CG6 | 128×192 | 2 | 32 | PMODE 4 |
| 7 | RG6 | 256×192 | 1 | 32 | — |

**2bpp color sets (4 colors per pixel pair):**
- CSS=0: Green, Yellow, Blue, Red (standard VDG palette)
- CSS=1: Black, Blue, Orange, White (NTSC artifact color approximation for TFT — pixel 0 mapped to Black instead of Buff/White so games using pixe
l 0 as "background" display correctly)

**1bpp color pairs:**
- CSS=0: Dark Green background / Bright Green foreground
- CSS=1: Dark Orange background / Bright Orange foreground

**Upscaling:** All modes are upscaled to 256 pixels wide in the line buffer. Scale factor = 256 / native_width (1× for RG6, 2× for 128-wide modes,
 4× for 64-wide modes).

### Rendering Pipeline

```
1. machine_run_scanline() sets vdg->row_address from SAM running counter
2. mc6847_render_scanline() reads VRAM at row_address:
   - AG=0: render_text_scanline() — font lookup + inverse/semigraphics
   - AG=1: render_graphics_scanline() — pixel unpacking + palette
3. Output: line_buffer[256] contains palette indices (0–11)
4. hal_video_render_scanline() converts palette indices to RGB565 → TFT sprite
```

### Mode Change Detection

`mc6847_set_mode()` is called by `update_vdg_mode()` in machine.cpp whenever PIA1 port B or SAM V0–V2 changes. It only logs when the mode actually
 changes (avoids debug spam during normal operation).

---

## SAM6883 — Address Multiplexer (`sam6883.h`, `sam6883.cpp`)

Emulates the SAM6883 Synchronous Address Multiplexer, which controls memory mapping, VDG display addressing, and CPU clock speed.

### Register Space ($FFC0–$FFDF)

The SAM uses 16 bit-pair registers (32 addresses). Each bit has two addresses:
- **Even address**: clear the bit
- **Odd address**: set the bit

Any write to the address triggers the action (the data byte is ignored).

| Bits | Name | Function |
|------|------|----------|
| 0–2 | V0–V2 | VDG graphics mode (maps to GM0–GM2) |
| 3–9 | F0–F6 | Display offset → base address = (F-value << 9) = F × 512 |
| 10 | P1 | Page select (all-RAM mode) |
| 11–12 | R0–R1 | CPU rate (not used in our emulation) |
| 13–14 | M0–M1 | Memory size |
| 15 | TY | Map type (ROM/RAM) |

### Register Write Operation

```cpp
sam6883_write(sam, addr):   // addr = offset from $FFC0 (0–31)
  bit_num = addr >> 1       // Which of the 16 bits
  set = addr & 1            // 0 = clear, 1 = set
  if set: reg |= (1 << bit_num)
  else:   reg &= ~(1 << bit_num)
  update_from_register()    // Recompute derived fields
```

`update_from_register()` extracts:
- `vdg_mode` = bits 0–2 (V0–V2)
- `vdg_base` = (bits 3–9) << 6 (64-byte granularity for display start)
- Divide counter parameters from lookup tables indexed by vdg_mode
- `mem_size`, `page1`, `ty` from upper bits

### VDG Address Counter

The SAM maintains a running address counter that feeds the VDG with display data addresses. This is the most complex part of the SAM emulation and
 must match XRoar exactly for all graphics modes to display correctly.

**Counter lifecycle:**

1. **Field sync (vsync)** — `sam6883_vdg_fsync(true)`: Resets counter to `vdg_base`, clears X and Y divide counters.
2. **Data fetch** — `sam6883_vdg_fetch_bytes(nbytes)`: Called once per active scanline with `bytes_per_row` for the current mode. Advances the cou
nter with divide-by-X/Y logic, processing in 16-byte chunks to match XRoar's `sam_vdg_bytes()` behavior.
3. **Horizontal sync** — `sam6883_vdg_hsync(false)`: Supplementary counter adjustment — adds `vdg_mod_add` bytes via divide logic, then clears ali
gnment bits.

**Divide-by-X/Y row repetition (indexed by GM value):**

| GM | X-div | Y-div | Mod-add | Bytes/Row | Effect |
|----|-------|-------|---------|-----------|--------|
| 0 | 1 | 12 | 16 | 16 | Each 16-byte row repeated 12× (64 rows → 192 scanlines) |
| 1 | 3 | 1 | 8 | 16 | Each 16-byte row repeated 3× (64 rows) |
| 2 | 1 | 3 | 16 | 32 | Each 32-byte row repeated 3× (64 rows) |
| 3 | 2 | 1 | 8 | 16 | Each 16-byte row repeated 2× (96 rows) |
| 4 | 1 | 2 | 16 | 32 | Each 32-byte row repeated 2× (96 rows) |
| 5 | 1 | 1 | 8 | 16 | Each 16-byte row 1× (192 rows) |
| 6 | 1 | 1 | 16 | 32 | Each 32-byte row 1× (192 rows) |
| 7 | 1 | 1 | 0 | 32 | Each 32-byte row 1× (no supplementary add) |

### Address Advancement with Divide Logic (`vdg_address_add`)

```
vdg_address_add(sam, n):
  new_addr = vdg_address + n
  if bit 4 flipped (crossed 16-byte boundary):
    xcount = (xcount + 1) % xdiv
    if xcount != 0:
      new_addr -= 0x10                // Rewind: stay on same 16-byte row
    else:
      if bit 5 flipped (crossed 32-byte boundary):
        ycount = (ycount + 1) % ydiv
        if ycount != 0:
          new_addr -= 0x20            // Rewind: stay on same 32-byte group
  vdg_address = new_addr
```

This implements XRoar's bit-boundary crossing logic. The X divider controls repetition at the 16-byte level; the Y divider controls repetition at
the 32-byte level. Together they produce the correct row repetition for all 8 graphics modes.

### Fetch Bytes Implementation

`sam6883_vdg_fetch_bytes()` processes the requested byte count in 16-byte aligned chunks. Within a 16-byte block, the address advances without div
ide logic. At each 16-byte boundary crossing, `vdg_address_add()` applies the divide counters.

```
fetch_bytes(nbytes):
  while nbytes > 0:
    b3_0 = address & 0x0F             // Position within 16-byte block
    chunk = 16 - b3_0                 // Remaining in current block
    if chunk > nbytes: chunk = nbytes
    if doesn't cross boundary:
      address += chunk                // Simple advance
    else:
      vdg_address_add(chunk)          // Apply divide logic
    nbytes -= chunk
```

### Why Two Advancement Steps?

In XRoar, the SAM counter advances in two places:
1. `sam_vdg_bytes()` — driven by the VDG data clock as it fetches bytes during active display
2. `sam_vdg_hsync()` — supplementary fixup at end of each scanline

Without the fetch step, modes like GM=7 (RG6, mod_add=0) never advance the counter — the display shows only the first row repeated 192 times. The
mod_add values are supplementary corrections, not the primary advancement mechanism.

### Clear Mask

After the hsync addition, `vdg_address &= vdg_mod_clear` clears low-order bits to realign the address:
```
GM: 0-5 → clear bits 1-4 (mask ~30 = ~0x1E) or bits 1-3 (mask ~14 = ~0x0E)
GM: 6   → clear bits 1-4 (mask ~30)
GM: 7   → no clear (mask ~0)
```

---

## Machine — System Integration (`machine.h`, `machine.cpp`)

The machine module wires all components into a complete CoCo 2 emulation. It owns all chip instances, memory buffers, and implements the 64KB addr
ess decoder.

### Machine Structure

```c
typedef struct Machine {
    MC6809   cpu;       // 6809 CPU
    MC6821   pia0;      // PIA at $FF00 (keyboard, vsync)
    MC6821   pia1;      // PIA at $FF20 (sound, VDG mode)
    MC6847   vdg;       // Video Display Generator
    SAM6883  sam;       // Address Multiplexer
    SV_DiskController fdc;  // WD1793 FDC at $FF40

    uint8_t* ram;           // 64 KB (PSRAM)
    uint8_t* rom_basic;     // 8 KB Color BASIC ($A000-$BFFF)
    uint8_t* rom_extbas;    // 8 KB Extended BASIC ($8000-$9FFF)
    uint8_t* rom_cart;      // 16 KB Disk BASIC ($C000-$FEFF)

    uint32_t scanline;      // Current scanline (0–261)
    uint32_t frame_count;
    uint32_t cycles_per_frame;  // 14916 for NTSC
    // ...
} Machine;
```

### Memory Map

```
$0000–$7FFF  RAM (64 KB, lower 32 KB directly visible)
$8000–$9FFF  Extended BASIC ROM (8 KB)   — or RAM when SAM TY=1
$A000–$BFFF  Color BASIC ROM (8 KB)      — or RAM when SAM TY=1
$C000–$FEFF  Disk BASIC / Cartridge ROM  — or RAM when SAM TY=1
$FF00–$FF1F  PIA0 (4 regs mirrored every 4 bytes)
$FF20–$FF3F  PIA1 (4 regs mirrored every 4 bytes)
$FF40–$FF5F  WD1793 Disk Controller (DSKREG + FDC regs)
$FF60–$FFBF  Reserved (reads $FF)
$FFC0–$FFDF  SAM control (write-only bit set/clear)
$FFE0–$FFFF  Interrupt vectors (from Color BASIC ROM $BFE0–$BFFF)
```

### SAM All-RAM Mode (MAP TYPE)

Writing to $FFDF sets the SAM MAP TYPE bit (`sam.ty = true`), enabling **all-RAM mode**. In this mode, $8000–$FEFF maps to RAM instead of ROM — the full 64 KB address space becomes read/write RAM. Writing to $FFDE clears the bit, restoring normal ROM mapping.

The I/O space ($FF00–$FFFF) is always hardware-decoded regardless of MAP TYPE, so PIA, FDC, SAM registers, and interrupt vectors continue to work normally.

This mode is required by **OS-9 Level 1**, which uses the DOS command to load a bootstrap from Track 34 into $2600, then the bootstrap enables all-RAM mode to copy itself into upper memory and load the OS-9 kernel.

### Memory Access (`machine_read` / `machine_write`)

The CPU's `read` and `write` function pointers point to `machine_read()` and `machine_write()`, which implement the full address decoder:

- **I/O space** ($FF00–$FFFF) is always checked first — hardware-decoded regardless of MAP TYPE
- **Reads** from I/O space perform side effects (PIA IRQ flag clearing, keyboard matrix scanning, FDC status)
- **Writes** to PIA1 trigger VDG mode updates and audio DAC output
- **Writes** to SAM update the VDG base address and mode bits
- **$8000–$FEFF**: when SAM TY=1, reads/writes go to RAM; when TY=0, reads return ROM and writes are ignored
- ROM vectors at $FFE0–$FFFF read from Color BASIC ROM ($BFE0–$BFFF)

### Keyboard Scanning (in `machine_read`)

When the CPU reads PIA0 data A (the ROM's KEYIN routine), `machine_read()` intercepts the read and:

1. Reads PIA0 port B output bits (column select, active low)
2. For each column driven low, calls `hal_keyboard_scan(col)` to get row data
3. ANDs all returned row data together (multi-column scan for key detection)
4. Reads joystick buttons into PA0 (right button) and PA1 (left button)
5. Performs joystick DAC comparator check for PA7 (see Joystick section below)
6. Calls `mc6821_set_input_a()` to set the row data for PIA to return

### Joystick Comparator (in `machine_read`)

The CoCo reads joystick positions through a DAC comparator loop. The machine layer implements this exactly matching XRoar's `joystick_update()`:

```
port = (PIA0 CRB bit 3) >> 3        // 0=right, 1=left joystick
axis = (PIA0 CRA bit 3) >> 3        // 0=X, 1=Y axis
dac  = (PIA1 DA & 0xFC) + 2         // 8-bit DAC value (range 2–254)
js   = hal_joystick_read_axis(port, axis) * 4 + 2  // Scale 0–63 → 2–254
PA7  = (js >= dac) ? 1 : 0          // Comparator result
```

Joystick ADC is refreshed every 16 scanlines (~16 times per frame) to balance responsiveness and overhead.

### Audio Output (in `machine_write`)

When the CPU writes to PIA1:
- **Port A write** (or CRA write): Extracts 6-bit DAC value from bits 2–7 of `data_a & ddr_a`, calls `hal_audio_write_dac()`.
- **Port B write** (or CRB write): Extracts single-bit audio from bit 1, calls `hal_audio_write_bit()`. Also updates VDG mode (AG from PB7, CSS fr
om PB3).

### VDG Mode Update (`update_vdg_mode`)

Called when PIA1 port B or SAM V0–V2 changes:
```
vdg_mode = 0
if PIA1 PB7 (via data_b & ddr_b): vdg_mode |= VDG_AG
if PIA1 PB3:                       vdg_mode |= VDG_CSS
vdg_mode |= SAM V0–V2 (bits 0–2)
mc6847_set_mode(&vdg, vdg_mode)
```

### IRQ Routing

```
PIA0 IRQA → mc6809_irq()    ← HS (not used by BASIC)
PIA0 IRQB → mc6809_irq()    ← FS / 60Hz vsync timer
PIA1 IRQA → mc6809_firq()   ← cartridge interrupt
PIA1 IRQB → mc6809_firq()   ← cartridge interrupt
```

Wired in `machine_init()` via static callback functions and re-wired after `machine_reset()` (since reset preserves callbacks but the callbacks mu
st be re-assigned to the PIA struct).

### Frame Execution

`machine_run_frame()` processes 262 NTSC scanlines per frame:

```
machine_run_frame(m):
  Precompute scanline_cycle_targets[262] for even distribution
  cycles_this_frame = 0, scanline = 0
  for 262 scanlines:
    machine_run_scanline(m)
  hal_video_present(ram, sam.vdg_base, vdg.mode)  // VRAM shadow compare + conditional push
  frame_count++
```

Each `machine_run_scanline()`:
1. Ticks FDC deferred INTRQ counter (`sv_disk_tick`)
2. Runs CPU for ~57 cycles (from precomputed target table, avoids per-scanline divide)
3. On scanline 0: VDG FS rising edge → PIA0 CB1 → IRQ possible; SAM counter reset
4. For active scanlines (0–191): SAM provides row address → VDG renders → HAL pushes to TFT sprite
5. SAM data fetch advances counter by `bytes_per_row` (text=32, graphics=table lookup)
6. SAM hsync performs supplementary counter adjustment
7. On scanline 192: VDG FS falling edge → PIA0 CB1 → 60Hz IRQ fires

### Memory Allocation

All large buffers (64 KB RAM, ROM images) are allocated from PSRAM when available (`ps_malloc`), falling back to heap. This keeps internal SRAM fr
ee for stack and DMA buffers. The helper `machine_alloc(size, label)` logs the allocation source.

### Initialization Sequence

1. `machine_init()`: Allocate memory (RAM, 3 ROM buffers from PSRAM), init all chips, wire CPU `read`/`write` callbacks and PIA IRQ routing callba
cks
2. `machine_load_roms()`: Load Color BASIC ($A000), Extended BASIC ($8000), and optionally Disk BASIC ($C000) from SD card via `hal_storage_load_f
ile()`
3. `machine_reset()`: Clear RAM, reset all chips, set VDG VRAM pointer, reset SAM counter, CPU reads reset vector from ROM

---

## HAL Integration — How Core Connects to Hardware

The Hardware Abstraction Layer (`src/hal/`) bridges the emulation core to the ESP32-S3 hardware. Each HAL module has a specific integration point
with the core.

### Video HAL (`hal_video.cpp`)

**Integration point**: Called from `machine_run_scanline()` for each active scanline, and `machine_run_frame()` at end of frame.

| HAL Function | Called From | Purpose |
|-------------|------------|---------|
| `hal_video_render_scanline(line, pixels, width)` | `machine_run_scanline()` | Converts VDG `line_buffer[256]` palette indices → RGB565, writes i
nto TFT_eSprite framebuffer |
| `hal_video_present(ram, vdg_base, vdg_mode)` | `machine_run_frame()` | VRAM shadow compare: skips SPI push if screen unchanged (OPT-16) |
| `hal_video_get_tft()` | `supervisor.cpp` | Returns TFT_eSPI pointer for OSD direct rendering |

**Data flow**: `VDG line_buffer[256]` (palette indices) → `palette_swapped[]` lookup → memcpy to sprite framebuffer → `sprite->pushSprite()` to TF
T.

**Three display scale modes** (compile-time `DISPLAY_SCALE_MODE` in config.h):
- Mode 0: 1:1 native 256×192 centered at (32,24) on 320×240 TFT
- Mode 1: Nearest-neighbor stretch to fill display minus border
- Mode 2: Fixed zoom factor centered on display

**VRAM shadow compare (OPT-16)**: Instead of blind frame skipping, `hal_video_present()` compares the current CoCo VRAM against a 6,144-byte shadow buffer. Unchanged frames skip the SPI push entirely (~3,500 us saved). On mode/base changes, 10 frames are force-pushed to capture multi-frame screen setup. Measured: 64 FPS text (static), 45 FPS graphics (static), 27 FPS graphics (scrolling). FPS overlay (F5 toggle) counts emulated frames and draws on TFT after each push.

### Audio HAL (`hal_audio.cpp`)

**Integration point**: Called from `machine_write()` when PIA1 registers change.

| HAL Function | Called From | Purpose |
|-------------|------------|---------|
| `hal_audio_write_dac(dac6)` | `machine_write()` PIA1 port A | 6-bit DAC value (0–63) scaled to 8-bit PWM duty. **Gated by sound MUX** (PIA1 CRB bit 3 + PIA0 CA2/CB2 source select) |
| `hal_audio_write_bit(value)` | `machine_write()` PIA1 port B | Single-bit audio → PWM 0 or 255 (independent of MUX) |

**Implementation**: ESP32-S3 has no internal DAC. Uses LEDC PWM at 78.125 kHz (8-bit resolution) on GPIO17. A hardware timer ISR at 22,050 Hz read
s `audio_current_level` and writes it to the PWM duty register via direct LEDC peripheral access (`LEDC.channel_group[0].channel[ch].duty.duty`).
The ISR uses `IRAM_ATTR` for timing-critical execution.

**Two audio paths** (matching real CoCo hardware):
1. **6-bit DAC**: PIA1 PA bits 2–7 → `(dac6 << 2) | (dac6 >> 4)` scales 0–63 to 0–255. **Gated by sound MUX**: only output when PIA1 CRB bit 3 is set (MUX enabled) and PIA0 CA2/CB2 select DAC source (both 0). BASIC clears PIA1 CRB bit 3 during `JOYSTK()` reads to mute the DAC (prevents audible noise from successive approximation ADC writes).
2. **Single-bit**: PIA1 PB bit 1 → 0 or 255 (full swing for cassette/beep). Independent of MUX.

See `docs/audio-hal.md` for full MUX architecture and implementation details.

### Keyboard HAL (`hal_keyboard.cpp`)

**Integration point**: Called from `machine_read()` when CPU reads PIA0 data A register.

| HAL Function | Called From | Purpose |
|-------------|------------|---------|
| `hal_keyboard_scan(column)` | `machine_read()` PIA0 port A | Returns row data for given column (active-low) |
| `hal_keyboard_tick()` | `hal_process_input()` each frame | Decrements deferred release counters |

**Input pipeline**: USB keyboard (physical) → ESP-IDF USB Host HID (Core 0 FreeRTOS task) → 8-byte boot protocol reports → `process_keyboard_repor
t()` diffs prev/current → FreeRTOS queue → `hid_host_process()` on Core 1 → `on_hid_key()` callback → HID usage lookup in `KEY_MAP[]` → `set_key(c
ol, row, pressed)` modifies `key_matrix[8]`.

**Matrix representation**: `key_matrix[col]` bit `row` = 0 if pressed (active LOW). When the CPU scans column N by driving PIA0 PB low, `hal_keybo
ard_scan(N)` returns the corresponding row byte.

**Deferred release**: Keys must stay held for `MIN_HOLD_FRAMES = 4` frames minimum so the CoCo ROM's KEYIN routine can detect them. Without this,
fast key taps (press+release in the same USB poll) are invisible to the emulated CPU.

**Hotkey layer**: Before reaching the CoCo matrix, key events are intercepted for:
- F1 → supervisor toggle (always)
- F2 → machine reset + disk flush (emulation mode only)
- F3 → quick mount last disk (emulation mode only)
- F5 → FPS overlay toggle (emulation mode only)
- When supervisor is active, all keys route to `supervisor_on_key()` instead of the matrix.

### Joystick HAL (`hal_joystick.cpp`)

**Integration point**: Called from `machine_read()` during PIA0 port A reads.

| HAL Function | Called From | Purpose |
|-------------|------------|---------|
| `hal_joystick_read_axis(port, axis)` | `machine_read()` | Returns 6-bit axis value (0–63) |
| `hal_joystick_read_button(port)` | `machine_read()` | Returns 1 if button pressed |
| `hal_joystick_update()` | `hal_process_input()` + `machine_read()` | Refreshes ADC readings |

**Hardware**: Two analog joysticks via ESP32 ADC. Pins: JOY0 X=GPIO9, Y=GPIO8, BTN=GPIO18; JOY1 X=GPIO1, Y=GPIO6, BTN=GPIO7. Uses the `CoCoJoystic
k` library for calibration, dead zone, and debounce.

**Comparator emulation**: The CoCo reads joystick positions through successive approximation — BASIC ROM sweeps the DAC value from 0 to 63 and che
cks PA7 each time. The machine layer computes `(axis_value * 4 + 2) >= (dac_value)` on each PIA0 read, matching XRoar's `joystick_update()` exactl
y.

**ADC refresh rate**: Every 16 scanlines (~16 reads per frame) to balance responsiveness and ADC overhead. Tracked via `last_joy_scanline` to avoi
d redundant reads within the same 16-scanline slot.

### Storage HAL (`hal_storage.cpp`)

**Integration point**: Called from `machine_load_roms()` at boot, and from `sv_disk_mount()` for disk images.

| HAL Function | Called From | Purpose |
|-------------|------------|---------|
| `hal_storage_init()` | `hal_init()` | SD card init on dedicated HSPI bus |
| `hal_storage_load_file(path, buf, size)` | `machine_load_roms()`, `sv_disk_mount()` | Reads file from SD into buffer |
| `hal_storage_file_exists(path)` | File browser, ROM loader | Checks if file exists on SD |

**SD bus isolation**: Uses dedicated HSPI (SPI3) peripheral — separate from TFT_eSPI which uses default FSPI (SPI2). Pins: SCLK=39, MOSI=40, MISO=
41, CS=38. This prevents DMA conflicts between SD reads and TFT writes.

**Boot sequence constraint**: `hal_storage_init()` runs before `hal_video_init()` because both use SPI peripherals. SD card must be mounted before
 TFT takes over.

---

## Module Interaction Diagram

```
                     ┌──────────────┐
                     │  machine.cpp │ (address decoder + frame loop)
                     └──────┬───────┘
            ┌───────────────┼───────────────┐
            │               │               │
     ┌──────▼──────┐ ┌─────▼─────┐  ┌──────▼──────┐
     │   MC6809    │ │  MC6821   │  │   MC6847    │
     │   (CPU)     │ │ PIA0+PIA1 │  │   (VDG)     │
     └──────┬──────┘ └─────┬─────┘  └──────┬──────┘
            │               │               │
  read/write callbacks  IRQ/FIRQ        vram pointer
            │           callbacks           │
            │               │               │
            └───────┬───────┘        ┌──────┘
                    │                │
             ┌──────▼──────┐  ┌─────▼──────┐
             │  SAM6883    │  │  HAL layer  │
             │ (addr mux)  │  │ (TFT, kbd,  │
             └─────────────┘  │  audio, SD) │
                              └────────────┘
```

### Signal Flow Summary

| Signal | From | To | Mechanism |
|--------|------|----|-----------|
| Memory read/write | CPU | machine.cpp | Function pointers (`cpu->read`, `cpu->write`) |
| IRQ | PIA0 IRQA/IRQB | CPU | Callback → `mc6809_irq()` |
| FIRQ | PIA1 IRQA/IRQB | CPU | Callback → `mc6809_firq()` |
| NMI | FDC INTRQ | CPU | Callback → `mc6809_nmi()` (edge-triggered) |
| HALT | FDC DRQ | CPU | `cpu->halted` flag via callback |
| 60Hz vsync | VDG FS | PIA0 CB1 | `mc6821_cb1_transition()` in scanline loop |
| VDG mode | PIA1 PB + SAM V0–V2 | VDG | `mc6847_set_mode()` via `update_vdg_mode()` |
| Display address | SAM counter | VDG | `vdg->row_address` set each scanline |
| Audio DAC | PIA1 PA | HAL audio | `hal_audio_write_dac()` on PIA1 write |
| Audio bit | PIA1 PB1 | HAL audio | `hal_audio_write_bit()` on PIA1 write |
| Keyboard | HAL keyboard | PIA0 PA | `hal_keyboard_scan()` on PIA0 data A read |
| Joystick | HAL joystick | PIA0 PA7 | `hal_joystick_read_axis()` → comparator in machine_read |
| Video out | VDG line_buffer | HAL video | `hal_video_render_scanline()` per active scanline |
| Frame push | machine | HAL video | `hal_video_present()` after 262 scanlines |
| Disk I/O | FDC | HAL storage | `sv_disk_mount()` loads .DSK into PSRAM cache |
