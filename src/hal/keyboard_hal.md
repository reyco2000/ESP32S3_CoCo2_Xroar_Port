# Keyboard HAL — ESP32_CoCo2_XRoar_Port

## Overview

Translates USB HID keyboard input into a CoCo 6x8 keyboard matrix that the emulated PIA0 can scan. A USB keyboard plugged into the ESP32-S3's OTG port is the sole physical input device.

**Files:**

| File | Purpose |
|---|---|
| `hal_keyboard.cpp` | HID-to-matrix mapping, hotkey intercept, deferred release, matrix state |
| `usb_kbd_host.h/.cpp` | ESP32-S3 USB Host HID driver, Core 0 tasks, FreeRTOS queue |
| `hal.cpp` | Calls `hid_host_process()` + `hal_keyboard_tick()` each frame |
| `hal.h` | Public API declarations |

---

## Architecture — Dual-Core Pipeline

```
USB keyboard ──GPIO19/20──► ESP32-S3 USB OTG PHY
                                │
                         ┌──────┴──────┐
                         │   Core 0    │
                         │  usb_host_  │
                         │  lib_task   │  ← USB host library event loop
                         │  (pri 2)    │
                         ├─────────────┤
                         │  HID host   │
                         │  bg task    │  ← Espressif HID class driver
                         │  (pri 5)    │
                         └──────┬──────┘
                                │ hid_host_interface_event_cb()
                                │ process_keyboard_report()
                                ▼
                    ┌───────────────────────┐
                    │  FreeRTOS Queue (32)  │  key_event_t {usage, modifiers, pressed}
                    │  xQueueSend (Core 0)  │
                    └───────────┬───────────┘
                                │ xQueueReceive (Core 1)
                                ▼
                         ┌──────┴──────┐
                         │   Core 1    │
                         │  loop()     │
                         │  hal_process│  ← hid_host_process() drains queue
                         │  _input()   │    → on_hid_key() callback
                         └──────┬──────┘
                                │
                    ┌───────────┴───────────┐
                    │ Layer 1: Hotkey check  │ F1-F5 intercepted
                    │ Layer 2: Supervisor or │ OSD gets keys if active
                    │          CoCo matrix   │ else → matrix injection
                    └───────────────────────┘
```

**Key constraint:** USB Host uses the ESP32-S3's USB PHY (GPIO19=D-, GPIO20=D+). This means **USB CDC Serial cannot coexist** — serial monitor must use UART, not USB. Board settings: `USBMode=default`, `CDCOnBoot=default`.

---

## USB Host Layer (`usb_kbd_host.cpp`)

### Initialization (`hid_host_begin`)

1. Creates FreeRTOS queue (32 entries of `key_event_t`)
2. Installs USB Host Library (`usb_host_install`)
3. Spawns `usb_host_lib_task` on **Core 0, priority 2** — handles USB library events
4. Installs HID Host driver (`hid_host_install`) with background task on **Core 0, priority 5** — handles HID class events

### Device Connection Flow

```
USB device plugged in
  → hid_host_device_event_cb(HID_HOST_DRIVER_EVENT_CONNECTED)
    → check proto == HID_PROTOCOL_KEYBOARD
    → hid_host_device_open() + set boot protocol + device_start()
    → s_connected = true
```

### Report Processing (`process_keyboard_report`)

Receives **8-byte boot protocol reports** (standard USB HID keyboard):

```
Byte 0: modifier bitmask (Ctrl/Shift/Alt/GUI)
Byte 1: reserved (always 0)
Bytes 2-7: up to 6 simultaneous keycodes (HID usage IDs)
```

**Diff algorithm:** Compares current report against `s_prev_keys[6]` to detect:
- **New keys** (in current but not in previous) → enqueue press event
- **Released keys** (in previous but not in current) → enqueue release event
- **Modifier changes** (`data[0] != s_prev_modifier`) → enqueue modifier event with `usage=0`

### Disconnect

`hid_host_interface_event_cb(DISCONNECTED)` → `s_connected = false`, device closed.

### Queue Drain (`hid_host_process`)

Called from `hal_process_input()` on Core 1 each frame. Non-blocking `xQueueReceive` loop dispatches all pending events to the registered callback (`on_hid_key`).

---

## Keyboard HAL Layer (`hal_keyboard.cpp`)

### Key Event Routing (`on_hid_key`)

Three-layer dispatch on every key event:

**Layer 1 — Global hotkeys** (press only, always active):

| Key | HID | Action |
|---|---|---|
| F1 | `0x3A` | `supervisor_toggle()` |

**Layer 1b — Emulation hotkeys** (press only, supervisor must be inactive):

| Key | HID | Action |
|---|---|---|
| F2 | `0x3B` | `machine_reset()` — soft reset |
| F3 | `0x3C` | `supervisor_quick_mount_last_disk()` |
| F4 | `0x3D` | Reserved (future debugger) |
| F5 | `0x3E` | `hal_video_toggle_fps_overlay()` |

**Layer 2a — Supervisor input** (when OSD is active):
All non-F1 keys forwarded to `supervisor_on_key(usage, pressed)`. CoCo matrix sees nothing.

**Layer 2b — CoCo matrix injection** (when supervisor is inactive):
HID usage mapped to CoCo matrix position via `find_mapping()`, then injected into `key_matrix[]`.

### HID → CoCo Matrix Mapping

**CoCo keyboard matrix** (ROM-verified, 7 rows × 8 columns):

```
        PB0    PB1    PB2    PB3    PB4    PB5    PB6    PB7
PA0:     @      A      B      C      D      E      F      G
PA1:     H      I      J      K      L      M      N      O
PA2:     P      Q      R      S      T      U      V      W
PA3:     X      Y      Z     UP    DOWN   LEFT  RIGHT   SPACE
PA4:     0      1      2      3      4      5      6      7
PA5:     8      9      :      ;      ,      -      .      /
PA6:   ENTER  CLEAR  BREAK  (n/a)  (n/a)  (n/a)  (n/a)  SHIFT
```

**Matrix encoding:** `key_matrix[col]` is indexed by PB column (0-7). Each bit represents a PA row (0-6). **Active LOW** — bit=0 means pressed.

**`KeyMap` struct:**
```cpp
struct KeyMap {
    uint8_t hid_usage;       // USB HID usage code
    uint8_t req_modifier;    // 0=any, else modifier must be held
    uint8_t col;             // PB column (0-7)
    uint8_t row;             // PA row (0-6)
    bool    needs_shift;     // Assert CoCo SHIFT
    bool    suppress_shift;  // Suppress CoCo SHIFT (PC shift produced this)
};
```

**`find_mapping()` priority:** If a modifier-specific entry matches (e.g. Shift+2 → @), it wins over the generic entry (2 → digit 2).

**Notable PC-to-CoCo translations:**

| PC Key | HID | CoCo Key | Notes |
|---|---|---|---|
| Backspace | `0x2A` | CLEAR | PA6,PB1 |
| ESC | `0x29` | BREAK | PA6,PB2 |
| Pause | `0x48` | BREAK | PA6,PB2 |
| Insert | `0x49` | CLEAR | PA6,PB1 |
| Delete | `0x4C` | CLEAR | PA6,PB1 |
| Shift+2 | `0x1F`+MOD | @ | PA0,PB0, suppress CoCo SHIFT |
| Shift+; | `0x33`+MOD | : | PA5,PB2, suppress CoCo SHIFT |

### SHIFT Handling

CoCo SHIFT is a matrix position (PA6, PB7), not a modifier flag. The HAL translates:

- **PC Shift held + normal key:** CoCo SHIFT asserted in matrix alongside the key
- **`suppress_shift` flag:** For keys like `@` that require PC Shift but NOT CoCo SHIFT
- **`needs_shift` flag:** For keys that need CoCo SHIFT regardless of PC modifier state
- **Modifier-only events** (`usage=0`): Update CoCo SHIFT directly from modifier byte

### Deferred Key Release

**Problem:** CoCo BASIC's KEYIN routine scans the matrix over multiple frames with debouncing. A fast USB key tap (press+release in same frame) is invisible to the emulated CPU.

**Solution:** `DeferredRelease` system holds keys in the matrix for `MIN_HOLD_FRAMES = 4` frames before releasing.

```cpp
struct DeferredRelease {
    uint8_t col, row;
    uint8_t frames_left;  // 0 = slot free
};
```

- **`defer_release(col, row)`** — queues a release (up to `MAX_DEFERRED = 8` slots)
- **`tick_deferred_releases()`** — called once per frame via `hal_keyboard_tick()`, decrements counters, releases when zero
- If a key is re-pressed before its deferred release fires, the counter is refreshed

### PIA Integration

The emulated PIA0 calls `hal_keyboard_scan(column)` during its port-A read:

```cpp
uint8_t hal_keyboard_scan(uint8_t column) {
    return key_matrix[column];  // Active LOW: 0xFF = no keys pressed
}
```

The PIA selects columns via PB0-PB7 output, then reads PA0-PA6 input. BASIC's KEYIN routine scans all 8 columns and computes `key_code = PA_row * 8 + PB_column`.

---

## Public API Summary

| Function | File | Called from | Purpose |
|---|---|---|---|
| `hal_keyboard_init()` | hal_keyboard.cpp | `hal_init()` | Reset matrix, init deferred releases, start USB host |
| `hal_keyboard_tick()` | hal_keyboard.cpp | `hal_process_input()` | Tick deferred releases (once per frame) |
| `hal_keyboard_scan(col)` | hal_keyboard.cpp | PIA0 port-A read | Return matrix column (active LOW) |
| `hal_keyboard_set_machine(m)` | hal_keyboard.cpp | `setup()` | Wire Machine pointer for F2/F3 hotkeys |
| `hal_keyboard_press(row,col)` | hal_keyboard.cpp | Test code | Software key injection |
| `hal_keyboard_release(row,col)` | hal_keyboard.cpp | Test code | Software key release |
| `hal_keyboard_release_all()` | hal_keyboard.cpp | Init / reset | Clear entire matrix to 0xFF |
| `hid_host_begin(cb)` | usb_kbd_host.cpp | `hal_keyboard_init()` | Start USB host stack + HID driver |
| `hid_host_process()` | usb_kbd_host.cpp | `hal_process_input()` | Drain key queue (Core 0 → Core 1) |
| `hid_host_is_connected()` | usb_kbd_host.cpp | Diagnostic | Check if keyboard is enumerated |

---

## Hardware Requirements

| Signal | GPIO | Notes |
|---|---|---|
| USB D- | 19 | ESP32-S3 native USB OTG |
| USB D+ | 20 | ESP32-S3 native USB OTG |
| VBUS | 5V | Must be supplied externally (host mode) |

**Board settings:** `USBMode=default`, `CDCOnBoot=default` — required so USB PHY is available for Host mode. Serial monitor via UART only.

**Library:** `ESP32_USB_Host_HID` by esp32beans ([GitHub](https://github.com/esp32beans/ESP32_USB_Host_HID)) — wraps Espressif USB Host HID class driver.

---

## Troubleshooting

### Keyboard not detected
1. Check VBUS 5V supply — USB host must provide power to the keyboard
2. Check serial log for `[USB_KBD] USB Host + HID driver started on Core 0`
3. Look for `HID Device Connected` / `Keyboard ready` in log
4. Verify `USBMode=default` and `CDCOnBoot=default` in board config — if CDC is enabled, USB PHY is taken

### Keys not registering in BASIC
1. Check `hid_host_is_connected()` returns true
2. Verify `hal_keyboard_scan()` is being called by PIA0 (check PIA port-B output selects columns)
3. Check deferred release timing — if `MIN_HOLD_FRAMES` is too low, KEYIN may miss keys (current: 4 frames)
4. For shifted characters (@, :), verify `suppress_shift` logic in `KEY_MAP`

### Keys stuck / ghost keys
1. Check `deferred_releases[]` — if all 8 slots full, releases happen immediately (may cause missed releases if queue overflows)
2. USB disconnect without release events → call `hal_keyboard_release_all()` on disconnect
3. Modifier-only events (`usage=0`) should update SHIFT state — check `s_prev_modifier` diff

### Supervisor eats all keys
- F1 toggles supervisor on/off. When active, ALL keys route to `supervisor_on_key()` — CoCo matrix is frozen
- If stuck, F1 always works (checked before supervisor routing)

### USB transfer errors
- `HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR` logged → usually electrical (bad cable, insufficient power)
- Queue overflow (32 entries) → keys dropped silently. Increase queue size in `hid_host_begin()` if needed

### Adding a new hotkey
1. Choose unused F-key HID code (F4=`0x3D` is reserved, F6=`0x3F` and up are free)
2. Add `if (usage == 0xNN)` block in `on_hid_key()` Layer 1b section
3. Use `extern void your_function(void);` to call across modules
4. Return after handling to prevent key from reaching CoCo matrix

### Adding a new CoCo key mapping
1. Find the HID usage code (see USB HID Usage Tables spec)
2. Find the CoCo matrix position (row=PA, col=PB) from the matrix table above
3. Add a `KeyMap` entry to `KEY_MAP[]` array
4. Set `needs_shift`/`suppress_shift` as needed for modifier translation
