# Color Computer 2 Emulator for ESP32-S3

A full **TRS-80 Color Computer 2 (CoCo 2)** emulator running on an ESP32-S3 microcontroller. Ported from the [XRoar](http://www.6809.org.uk/xroar/) emulator by Ciaran Anscomb.

**Beta-1 — March 2026**

## Features

- **Complete MC6809 CPU** emulation with accurate cycle counts
- **MC6847 VDG** video display — text and all semigraphics/graphics modes
- **Dual 6821 PIA** chips for keyboard, joystick, and audio I/O
- **SAM6883** address multiplexer for memory mapping and video control
- **WD1793 floppy disk controller** with .DSK and .VDK image support
- **USB HID keyboard** input via ESP32-S3 native USB host
- **Original CoCo Dual analog joysticks** with button support
- **On-Screen Display (OSD)** supervisor for disk mounting, machine reset, and status
- **PSRAM disk caching** — entire disk images loaded into PSRAM for zero-latency access
- **Multiple display support** — ILI9341, ST7789, or ST7796 SPI TFT panels
- **Audio output** via DAC or I2S
- **64 KB RAM**, configurable to 16/32/64 KB
- **25 FPS**, currently rendering 25 FPS on TFT display
- **Debugger**, A debugger to dump coco memory to serial port on Motorola-S or Intel Hex format

## Hardware Requirements

- **ESP32-S3-DevKitC-1** (dual-core, 8 MB PSRAM, USB OTG)
- **SPI TFT Display** — ST7789 320x240 recommended (ILI9341 or ST7796 also supported)
- **MicroSD card module** (SPI interface, FAT32 formatted)
- **USB keyboard** (standard HID, connected via USB OTG port)
- **Analog joysticks** (optional, two supported)

## Wiring Diagram

```
                             ESP32-S3-DevKitC-1
                          +----------------------+
                          |      [ Antenna ]     |
                          |  +----------------+  |
  [To TFT VCC] <--- 3.3V -|  | ESP32-S3-WROOM |  |- GND
                    3.3V -|  |                |  |- GPIO43
                   RESET -|  +----------------+  |- GPIO44
[To TFT RESET]<---GPIO04 -| 4                  1 |- GPIO01
 [To TFT LED] <---GPIO05 -| 5                  2 |- GPIO02 ---> [To TFT DC]
                  GPIO06 -| 6                 42 |- GPIO42
 [To Joy2 Btn] <--GPIO07 -| 7                 41 |- GPIO41 ---> [To SD MISO]
   [To Joy2 Y]<-- GPIO15 -| 15                40 |- GPIO40 ---> [To MOSI SD]
   [To Joy2 X]<-- GPIO16 -| 16                39 |- GPIO39 ---> [To SD SCK]
                  GPIO17 -| 17                38 |- GPIO38 ---> [To SD CS]
 [To Joy1 Btn] <--GPIO18 -| 18                37 |- GPIO37
   [To Joy1 Y] <--GPIO08 -| 8                 36 |- GPIO36
                   GPIO3 -| 3       [RGB]     35 |- GPIO35
                  GPIO46 -| 46                 0 |- GPIO0  (BOOT)
   [To Joy1 X] <--GPIO09 -| 9                 45 |- GPIO45
   [To TFT CS] <--GPIO10 -| 10                48 |- GPIO48
  [To TFT SDI] <--GPIO11 -| 11                47 |- GPIO47
  [To TFT SCK] <--GPIO12 -| 12                21 |- GPIO21
  [To SD MISO] <--GPIO13 -| 13                20 |- GPIO20
    [To SD CS] <--GPIO14 -| 14                19 |- GPIO19
                      5V -| 5V                 G |- GND
  [To TFT GND] <---- GND -| G                  G |- GND
                          |                      |
                          |    BOOT      RESET   |
                          |    [O]        [O]    |
                          |                      |
                          |   [UART]     [USB]   |
                          +---|====|-----|====|--+
                                            |
                                     Connect USB Keyboard here
(Due to power consumption I have used a USB Dongle keyboard that uses less power)


                TFT ST7789 Display                          SD Card Module
             +----------------------+                  +----------------------+
             |                      |                  |                      |
             |  VCC   <--- [3.3V]   |                  |  VCC   <--- [3.3V]   |
             |  GND   <--- [GND]    |                  |  GND   <--- [GND]    |
             |  CS    <--- [GPIO10] |                  |  CS    <--- [GPIO38] |
             |  RESET <--- [GPIO04] |                  |  SCK   <--- [GPIO39] |
             |  DC    <--- [GPIO02] |                  |  MOSI  <--- [GPIO40] |
             |  SDI   <--- [GPIO11] |                  |  MISO  <--- [GPIO41] |
             |  SCK   <--- [GPIO12] |                  |                      |
             |  LED   <--- [GPIO05] |                  |                      |
             |  MISO (Not connected)|                  |     [ MicroSD ]      |
             |                      |                  |                      |
             |   [ LCD SCREEN ]     |                  |                      |
             |                      |                  |                      |
             +----------------------+                  +----------------------+

                  Joystick 1                                Joystick 2
             +----------------------+                  +----------------------+
             |                      |                  |                      |
             |  X-Axis <--- [GPIO09]|                  |  X-Axis <--- [GPIO16]|
             |  Y-Axis <--- [GPIO08]|                  |  Y-Axis <--- [GPIO15]|
             |  Btn    <--- [GPIO18]|                  |  Btn    <--- [GPIO07]|
             |                      |                  |                      |
             +----------------------+                  +----------------------+
```

## Pin Reference

| Function      | Pin     | Notes                          |
|---------------|---------|--------------------------------|
| TFT CS        | GPIO10  | Display chip select            |
| TFT DC        | GPIO02  | Data/Command                   |
| TFT RST       | GPIO04  | Display reset                  |
| TFT MOSI/SDI  | GPIO11  | SPI data                       |
| TFT SCK       | GPIO12  | SPI clock                      |
| TFT Backlight | GPIO05  | PWM backlight control          |
| SD CS          | GPIO38  | SD card chip select            |
| SD MOSI        | GPIO40  | SD SPI data out                |
| SD MISO        | GPIO41  | SD SPI data in                 |
| SD SCK         | GPIO39  | SD SPI clock                   |
| Joystick 1 X   | GPIO09  | Analog axis                    |
| Joystick 1 Y   | GPIO08  | Analog axis                    |
| Joystick 1 Btn | GPIO18  | Digital button                 |
| Joystick 2 X   | GPIO16  | Analog axis                    |
| Joystick 2 Y   | GPIO15  | Analog axis                    |
| Joystick 2 Btn | GPIO07  | Digital button                 |
| Audio DAC      | GPIO17  | Analog audio output            |
| USB D-/D+      | GPIO19/20 | Native USB OTG (keyboard)   |

> The TFT and SD card use **separate SPI buses** (FSPI and HSPI) so there is no bus contention during emulation.
>
> To be able to use USB port on the **ESP32S3 DEV board need to solder USB OTG in the back of devboard**, (https://danielmangum.com/posts/usb-otg-esp32s3/?_x_tr_sl=en&_x_tr_tl=es&_x_tr_hl=es&_x_tr_pto=tc)  

## SD Card Setup

Format your MicroSD card as **FAT32** and create the following structure:

```
/
├── roms/
│   ├── bas13.rom          # Color BASIC 1.3 (required)
│   ├── extbas11.rom       # Extended BASIC 1.1 (required)
│   └── disk11.rom         # Disk BASIC 1.1 (required for floppy support)
└── (your .DSK and .VDK disk images anywhere on the card)
```

### ROM Files

| File           | Size | Description                      |
|----------------|------|----------------------------------|
| `bas13.rom`    | 8 KB | Color BASIC 1.3 (primary)        |
| `bas12.rom`    | 8 KB | Color BASIC 1.2 (fallback)       |
| `extbas11.rom` | 8 KB | Extended BASIC 1.1 (primary)     |
| `extbas10.rom` | 8 KB | Extended BASIC 1.0 (fallback)    |
| `disk11.rom`   | 8 KB | Disk BASIC 1.1 (primary)         |
| `disk10.rom`   | 8 KB | Disk BASIC 1.0 (fallback)        |

ROM files are validated by CRC-32 on startup. Fallback ROMs are loaded automatically if primary versions are not found.

### Supported Disk Formats

- **`.DSK`** (JVC format) — fully supported
- **`.VDK`** (Virtual Disk with 12-byte header) — fully supported

## Build & Flash

This is an **Arduino IDE** project (not PlatformIO).

### 1. Install Arduino IDE

Download [Arduino IDE 2.x](https://www.arduino.cc/en/software).

### 2. Add ESP32 Board Support

- Go to **File > Preferences** and add the ESP32 board manager URL
- Go to **Tools > Board > Board Manager**, search for **esp32** by Espressif, and install it **(Tested with esp32:esp32 3.3.3)**

### 3. Select Board Settings

| Setting            | Value                    |
|--------------------|--------------------------|
| Board              | ESP32S3 Dev Module       |
| CPU Frequency      | 240 MHz                  |
| USB Mode           | USB-OTG (TinyUSB)        |
| USB CDC On Boot    | Disabled                 |
| PSRAM              | OPI PSRAM                |
| Flash Size         | 8 MB / 16 MB (match your board) |
| Partition Scheme   | Default or Huge App      |

> Serial Monitor uses **UART** (not USB), since the USB PHY is dedicated to keyboard host mode.

### 4. Install Required Libraries

Via **Sketch > Include Library > Manage Libraries**:

- **TFT_eSPI** by Bodmer — configure `User_Setup.h` for your display type and pins (Tested  using TFT_eSPI 2.5.43)
- **SD** (built-in Arduino library)
- **ESP32_USB_Host_HID** by esp32beans

### 5. Configure

Edit `config.h` to match your hardware:

- `DISPLAY_TYPE` — 0 = ILI9341, 1 = ST7789, 3 = ST7796
- `DISPLAY_SCALE_MODE` — 0 = 1:1 centered, 1 = scaled fill, 2 = zoom
- `PIN_*` constants — adjust if your wiring differs
- `RAM_SIZE_KB` — 16, 32, or 64

Under Arduino1.8.19\libraries\TFT_eSPI **Replace User_Setup.h with templates based on display type** see folder /templates_for_TFT_eSP

### 6. Compile & Upload

- **Sketch > Verify/Compile** to build
- **Sketch > Upload** via USB
- Open **Serial Monitor at 115200 baud** to see startup diagnostics


## Keyboard Controls

| Key | Function |
|-----|----------|
| F1  | Toggle OSD (supervisor menu) |
| F2  | Machine reset |
| F3  | Quick mount disk |
| F5  | Toggle FPS display |

The USB keyboard is mapped to the CoCo's 7x8 key matrix. Standard alphanumeric keys, arrow keys, Enter, Shift, and common symbols work as expected.

## On-Screen Display (OSD)

Press **F1** to open the supervisor overlay. From here you can:

- **Mount/Eject Disks** — browse the SD card and mount .DSK/.VDK images to drives 0–3
- **Disk Manager** — view mounted drives and eject disks
- **Reset Machine** — warm or cold reset with confirmation
- **About** — version info and free memory

The OSD uses a CoCo green phosphor theme. Disk images are fully cached in PSRAM on mount for zero-latency access during emulation.

## Architecture

The emulator runs on the ESP32-S3's **dual cores**:

- **Core 0** — USB host event loop (keyboard HID processing)
- **Core 1** — Main emulation loop (CPU, video, audio, OSD)

Key events are passed between cores via a lock-free FreeRTOS queue.

### Emulation Loop

```
loop():
  1. hal_process_input()        — read keyboard queue, joystick ADC
  2. supervisor_update_and_render()  — if OSD active, render and return
  3. machine_run_frame()        — emulate 262 scanlines (14,916 CPU cycles)
  4. hal_render_frame()         — push framebuffer to TFT via SPI
```

### CoCo 2 Memory Map

```
$0000–$7FFF   RAM (up to 64 KB with SAM paging)
$8000–$9FFF   Extended BASIC ROM (8 KB)
$A000–$BFFF   Color BASIC ROM (8 KB)
$C000–$FEFF   Cartridge / Disk BASIC ROM (16 KB)
$FF00–$FF3F   PIA registers (keyboard, audio, VDG control)
$FF40–$FF5F   Disk controller (WD1793)
$FFC0–$FFDF   SAM control registers
```

### Architecture Diagrams

- `ESP32_COCO_CORE.png` — CPU, memory map, PIA, VDG, SAM wiring
- `DISK_HAL.png` — FDC, PSRAM disk cache, SD card interface

## Project Structure

```
CoCo_ESP32.ino              Main Arduino sketch (setup/loop)
config.h                    Hardware and build configuration
src/
├── core/                   Emulation core
│   ├── machine.h/cpp         CoCo machine — memory map, chip wiring, interrupts
│   ├── mc6809.h/cpp          MC6809 CPU — full opcode set with cycle-accurate timing
│   ├── mc6809_opcodes.h      Opcode table and cycle counts
│   ├── mc6821.h/cpp          6821 PIA — peripheral I/O
│   ├── mc6847.h/cpp          MC6847 VDG — scanline video rendering
│   └── sam6883.h/cpp         SAM — address multiplexing and clock control
├── hal/                    Hardware Abstraction Layer
│   ├── hal.h/cpp             HAL dispatcher (init, input, render)
│   ├── hal_video.cpp         TFT display output and scaling
│   ├── hal_audio.cpp         Audio DAC/I2S output
│   ├── hal_keyboard.cpp      USB HID to CoCo matrix mapping
│   ├── hal_joystick.cpp      Analog joystick input
│   ├── hal_storage.cpp       SD card access
│   ├── usb_kbd_host.h/cpp    USB host driver (runs on Core 0)
│   └── CoCoJoystick.h/cpp    Joystick class with calibration
├── supervisor/             On-Screen Display system
│   ├── supervisor.h/cpp      OSD lifecycle and state machine
│   ├── sv_menu.h/cpp         Menu definitions and actions
│   ├── sv_disk.h/cpp         WD1793 FDC emulation and PSRAM cache
│   ├── sv_filebrowser.h/cpp  SD card file browser
│   └── sv_render.h/cpp       OSD rendering (green phosphor theme)
├── roms/
│   └── rom_loader.h/cpp      ROM loading with CRC-32 validation
├── tests/
│   └── integration_test.h/cpp  LOADM binary verification
└── utils/
    └── debug.h               Debug output macros
```

## Known Limitations

- No TFT framebuffer readback (no MISO) — brief black flash when closing OSD
- CoCo Joystic uses 
- DMK disk format is recognized but not mountable
- Max 128 file entries in the SD card browser
- Machine selection is fixed to CoCo 2 (Dragon/CoCo 1 stubs only)
- Settings screen not yet implemented
- Sound does not match CoCo's frequency
- Integration (Demo Mode) on going
- NTSC TV Emulation stub only

## Credits

- **Reinaldo Torres / CoCo Byte Club** — ESP32 port and hardware design
- **Ciaran Anscomb** — [XRoar](http://www.6809.org.uk/xroar/) CoCo/Dragon emulator (original source)
- **Claude Code (Anthropic)** — co-development of the ESP32 port
- **Bodmer** — [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) display library
- **esp32beans** — [ESP32_USB_Host_HID](https://github.com/esp32beans/ESP32_USB_Host_HID) library

## License

This project is licensed under the **GNU General Public License v3.0**. See [LICENSE](LICENSE) for details.

