# KiCad Project — Schematic & PCB

Editable source design for the Doomsday Comms System (D.C.S., SKU OS2601),
board rev B.

## Files

| File / folder | Description |
|---------------|-------------|
| `dcs.kicad_pro` | KiCad project file — open this one |
| `dcs.kicad_sch` | Schematic |
| `dcs.kicad_pcb` | PCB layout (4-layer) |
| `fp-lib-table` | Footprint-library table (project-relative, points at `Misc.pretty`) |
| `Misc.pretty/` | Bundled custom footprint library (`GSC1031YB`) |
| `3D/` | Bundled component 3D models referenced by the PCB |

## Requirements

- **KiCad 7.0 or newer** (the project was authored in KiCad 7).

Open `dcs.kicad_pro` in KiCad, then open the schematic and PCB editors from
the project window.

## Self-contained — no external libraries needed

This project has **no dependency on any machine-specific library path**. It opens
complete on a fresh KiCad install:

- **Footprints** are embedded directly in `dcs.kicad_pcb` (every placed footprint
  carries its full pad/graphics definition), so the layout opens, edits, and
  fabricates without any library at all.
- **Symbols** are cached in `dcs.kicad_sch`, so the schematic opens standalone
  too.
- The one **custom footprint library** the design uses (`GSC1031YB`) is bundled
  in `Misc.pretty/`, and `fp-lib-table` references it with the project-relative
  `${KIPRJMOD}/Misc.pretty` — so you can also place that part into new layouts.
- The per-part **3D models** are bundled in `3D/`; the PCB references them with
  the project-relative `${KIPRJMOD}/3D/…`, so the 3D viewer resolves them with no
  setup. (For a single complete model of the assembled board, the STEP in
  [`../04_3D_Model/`](../04_3D_Model/) is also provided.)

All original absolute local paths were removed — nothing points outside this
folder. The `.kicad_prl` (per-user GUI state) and editor history were not
included.

## Key components

nRF52840 module (Ebyte E73-2G4M08S1C) · SX1262 LoRa (Ai-Thinker Ra-01SH) ·
ST7565 128×64 LCD (GMG12864-06D) · DRV2605L LRA haptics · MCP73833 LiPo charger ·
TLV75533 LDO · TM-1011A jog wheel.
