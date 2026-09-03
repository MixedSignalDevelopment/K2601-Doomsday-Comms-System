# Doomsday Comms System (D.C.S.) — Customer Files

**SKU:** OS2601 · **Board revision:** rev B · © Mixed Signal Development GmbH

This package contains everything needed to review, fabricate, assemble, and
re-flash the Doomsday Comms System board — a two-unit personal LoRa text
messenger built on an nRF52840 (BLE) + SX1262 (LoRa 868 MHz) with a 128×64 mono
LCD, jog-wheel input, LRA haptics, piezo, and USB-C LiPo charging.

## Contents

| Folder | What's inside |
|--------|---------------|
| [`01_KiCad_Project/`](01_KiCad_Project/) | Editable source design — KiCad 7 schematic + PCB layout |
| [`02_Gerbers/`](02_Gerbers/) | Fabrication data — RS-274X gerbers + Excellon drills (4-layer) |
| [`03_BOM/`](03_BOM/) | Bill of materials (rev B) with MPNs and LCSC part numbers |
| [`04_3D_Model/`](04_3D_Model/) | Full assembly 3D model (STEP) for enclosure/mechanical fit |
| [`05_Firmware/`](05_Firmware/) | Source firmware (PlatformIO) — bring-up tester + messenger apps |

Each folder has its own `README.md` with the specifics.

## Naming note

The customer-facing product is the **Doomsday Comms System (D.C.S.), SKU OS2601**.
The KiCad project and the firmware are fully rebranded to **dcs / OS2601**. The
**gerbers** and the **3D STEP model** still carry the internal design name
(`Messager-*` / `K2601`) in their filenames — this is the same board. Those were
left as-is because the gerber job manifest references its files by name, and the
names are irrelevant to fabrication.

## Ownership & licensing

- Design © 2026 **Mixed Signal Development GmbH**.
- **Firmware** is released under the **MIT License** — see
  [`05_Firmware/LICENSE`](05_Firmware/LICENSE).
- **Hardware** design files are provided for production of this board. If/when
  published as open hardware, the customary pairing is a hardware licence such
  as **CERN-OHL-S**.

## Excluded on purpose

- **Internal BOM** (with internal costing) — not included; only the production
  BOM is here.
- **Nordic s140 SoftDevice binary** — proprietary, not redistributable; see
  `05_Firmware/softdevice/README.md` for where to obtain it.
- Local machine paths embedded in the CAD files have been scrubbed.
