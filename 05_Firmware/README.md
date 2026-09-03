# Doomsday Comms System — D.C.S. (SKU OS2601) — bring-up firmware

Menu-driven peripheral tester for the rev-A board (nRF52840 + SX1262),
programmed and debugged over **SWD with an ST-Link V2** (no USB). This is
**not** Meshtastic yet — it is the "bare test firmware" used to prove out every
net before Meshtastic is layered on top. The pin map is encoded once, as a
proper Arduino board variant, so the same variant folder drops into a
Meshtastic build later.

Because there is no USB serial, the console runs over **SEGGER RTT** on the SWD
link: the firmware keeps a text ring-buffer in RAM and OpenOCD serves it as a
bidirectional terminal on `localhost:9090`.

```
platformio.ini                board/env definition + libraries + ST-Link/RTT
boards/dcs_nrf52840.json      custom PlatformIO board (Adafruit nRF52 core)
variants/dcs/                 variant.h + variant.cpp = the authoritative pin map
src/bringup/main.cpp          the bring-up tester (on-LCD wheel-driven menu)
lib/rtt/                       Arduino Stream wrapper over the core's SEGGER RTT
softdevice/                    where the s140 SoftDevice hex goes (not shipped, see below)
flash_softdevice.cfg          OpenOCD: mass-erase + flash the SoftDevice
openocd_rtt.cfg               OpenOCD script: attach ST-Link + serve RTT console
.vscode/tasks.json           SoftDevice flash + "RTT server" + "RTT console" tasks
```

## What it does

The whole tester runs **on the LCD**, driven by the thumbwheel — no host needed.
At boot it shows a splash, turns the backlight on, and drops into a scrolling
main menu. RTT (over SWD) just mirrors a boot/event log for debugging.

**Navigation** (only three inputs exist — wheel R, wheel L, wheel PUSH):

| input | action |
|-------|--------|
| roll R / L          | move the selection, or adjust a value |
| PUSH short (<700 ms)| select / activate |
| PUSH long (>700 ms) | back to the main menu |

> **rev-A note:** SW3's footprint was mis-wired; after a hardware bodge the
> switch works but on swapped pins — firmware maps **P0.12 = press,
> P0.07 = roll R, P0.13 = roll L** (`pollInputs()` in `main.cpp`). Rev-B should
> correct the SW3 footprint so no swap is needed. The RTT console keyboard also
> navigates (`w`/`s` = up/down, `Enter`/space = select, `b` = back), and the
> "Input diagnostic" menu item shows the raw switch pins.

**Menu items** (each maps to a step in the hardware bring-up order):

| item | what it tests | step |
|------|----------------|------|
| Display + contrast | patterns (frame / all-on / checker / stripes); roll adjusts contrast live | 3 |
| Backlight          | roll adjusts PWM brightness | 3 |
| Wheel test         | live R / L / PUSH counters + state | 4 |
| Haptic (LRA)       | roll picks DRV2605 ROM effect 1–123; push plays | 5 |
| Buzzer tone        | roll sets 2–5 kHz; push beeps (single-ended) | 5 |
| Buzzer sweep       | 3.6→4.4 kHz resonance sweep | 5 |
| Buzzer LOUD        | 4 kHz complementary (antiphase) drive, +6 dB | 5 |
| Battery            | live VBAT_MON in mV + bar + status | 7 |
| Radio status       | SX1262 SPI init (EU868, DIO2 switch, no TCXO); push = 1 TX (needs antenna) | 6 |
| Sleep (System OFF) | deep-sleep, wake on wheel press | — |

The board's safety rules are honoured in code: **TXEN (P0.28) is never
configured or driven**, `DIO2_AS_RF_SWITCH` is enabled, TCXO is disabled, and
sleep drops LRA_EN / backlight / LCD / radio before System OFF.

---

## Wiring the ST-Link V2

Connect the ST-Link V2 to the TC2030 / SWD header:

| ST-Link V2 pin | board      |
|----------------|------------|
| SWDIO          | SWDIO      |
| SWCLK          | SWDCLK     |
| GND            | GND        |
| 3.3V           | 3V3 (or power the board from the bench PSU / USB-C) |
| RST (optional) | P0.18 RESET|

The bench PSU can stand in for the battery as usual; the ST-Link only needs
SWDIO/SWCLK/GND to program and to carry the RTT console.

---

## Why a one-time SoftDevice flash is needed

This uses the Adafruit nRF52 core, whose only linker script places the app at
**0x26000** for the **s140 SoftDevice** memory layout. The app therefore cannot
boot unless the SoftDevice (with its MBR at 0x0) is present — a raw app-only
flash programs fine and verifies, but the CPU never reaches `setup()`.

We deliberately **skip the Adafruit UF2/DFU bootloader** (it refuses to
chain-load a raw-flashed app and just sits in DFU). Instead we flash
**MBR + SoftDevice only** (`softdevice/s140_6.1.1_mbr.hex`); with no UICR
bootloader pointer set, the MBR boots SoftDevice → app directly. This is a
one-time step (the equivalent of "flash the bootloader once"); the SoftDevice
survives every subsequent app upload.

> **The SoftDevice hex is not included in this repository.** Nordic's s140 is
> proprietary and is not redistributable here, so you must supply
> `softdevice/s140_6.1.1_mbr.hex` yourself before the flash step below — see
> [`softdevice/README.md`](softdevice/README.md) for where to obtain it.

> The chip does **not** need a pyOCD APPROTECT recovery — this board's nRF52840
> was not locked. If you ever hit a genuinely locked part, `python -m pyocd
> erase --mass -t nrf52840` clears it, but you won't normally need it.

---

## Run it from VS Code

### One-time setup
1. Open this `Firmware` folder in VS Code (**File → Open Folder**).
2. When prompted, install the recommended **PlatformIO IDE** extension
   (`platformio.platformio-ide`). It bundles its own toolchain (and OpenOCD) —
   you do **not** need Python or `pio` on your PATH. First launch downloads the
   nRF52 toolchain (a few minutes).
3. Reopen the folder once PlatformIO finishes initialising. A small **PlatformIO
   toolbar** (house / ✓ / → icons) appears in the blue status bar at the bottom.

### Build
- Click the **✓ (Build)** button in the status bar (or `Ctrl+Alt+B`).
- First build pulls U8g2, Adafruit DRV2605 and RadioLib automatically.
- Verified building clean here: RAM ~5%, Flash ~13%.

### Flash the SoftDevice — ONE TIME, before the first upload
- **Terminal → Run Task… → "Flash SoftDevice (one-time, before first upload)"**.
  This mass-erases and writes MBR + s140. Do it once per board (or after any
  mass-erase). Then continue to Upload below.

### Flash the app over SWD (ST-Link)
- Click the **→ (Upload)** button (or `Ctrl+Alt+U`). PlatformIO drives OpenOCD
  with the ST-Link (plain `swd` transport) to program the app at 0x26000. The
  SoftDevice underneath boots it. Re-run this for every code change.

### Open the RTT console (this is your bring-up terminal)
The console is not a COM port — it comes over SWD via RTT, in two steps:

1. **Terminal → Run Task… → "RTT: OpenOCD server (start first)"**
   — attaches the ST-Link and serves the console on `localhost:9090`.
   Leave this running.
2. **Terminal → Run Task… → "RTT console"**
   — opens the interactive terminal (PlatformIO monitor on `socket://localhost:9090`).

Now press `h` for the menu, then the single-letter commands in the table above.
Typing goes to the board; test output comes back. That terminal *is* your
bring-up console.

> One ST-Link can only be owned by one tool at a time. Upload first (releases
> the probe), then start the OpenOCD server task. If a task can't connect,
> make sure no debug session or other OpenOCD is still holding the probe.

### Source-level debugging (breakpoints)
ST-Link + nRF52 debugging is wired up: **Run → Start Debugging (F5)** launches
OpenOCD + GDB and halts at `setup()`. Set breakpoints, inspect variables, step.
Note this F5 session owns the ST-Link, so stop it before using the RTT tasks
(and vice-versa). For day-to-day bring-up the RTT console is the faster loop.

---

## Command-line equivalent (if you prefer)
PlatformIO installs a CLI at `~/.platformio/penv/Scripts/pio.exe`:
```
pio run                 # build
pio run -t upload       # flash over SWD via ST-Link
# then, for the console:
~/.platformio/packages/tool-openocd/bin/openocd -f openocd_rtt.cfg      # server
pio device monitor --port socket://localhost:9090 --echo               # terminal
```

## Troubleshooting

- **Flash "Verified OK" but nothing runs (no RTT, `setup()` never hit)** — the
  SoftDevice isn't present. Run the one-time **Flash SoftDevice** task, then
  Upload the app again. (Symptom when halted: PC sits below 0x26000.)
- **OpenOCD: "Debug adapter doesn't support 'hla_swd' transport"** — the platform
  default; this project already overrides it to plain `swd` in `platformio.ini`
  and the `.cfg` files. If you add your own OpenOCD invocation, use
  `-c "transport select swd"`, not `hla_swd`.
- **pyOCD `STLink error (29): Bad AP`** — pyOCD's AP scan tripping on this
  ST-Link when it probes the (absent) next access port; it does *not* mean the
  chip is locked. We use OpenOCD (not pyOCD) for flashing, which is unaffected.
- **RTT console is blank** — make sure the OpenOCD server task is running and
  the firmware is actually running (an F5 debug session halts at `setup()` — hit
  Continue). The console only prints once code runs.
- **"unknown command: rtt"** in the OpenOCD task — your bundled OpenOCD is old;
  update PlatformIO (`pio upgrade` / update the PlatformIO IDE extension), which
  pulls a newer `tool-openocd`.
- **Display garbled / shifted a few pixels** — GMG12864 panels vary; in
  `src/main.cpp` swap the `U8G2_ST7565_ERC12864_F_4W_SW_SPI` constructor for the
  `..._ALT_...` variant, or an `NHD_C12864` / `LM6059` profile, and re-tune
  `setContrast()`.
- **Probe busy** — only one of {Upload, F5 debug, RTT OpenOCD task} can own the
  ST-Link at a time. Stop the others first.

## Next step after bring-up
Once every net checks out, this `variants/dcs/` folder is copied into a
Meshtastic firmware checkout as a custom board variant, and the UI becomes a
custom Meshtastic module.

## License
Firmware in this repository is released under the **MIT License** (see
[`LICENSE`](LICENSE)) — © 2026 **Mixed Signal Development GmbH**. When the
hardware design files (schematics, PCB) are published, the customary pairing is
a hardware licence such as **CERN-OHL-S**; this repo covers the firmware only.
