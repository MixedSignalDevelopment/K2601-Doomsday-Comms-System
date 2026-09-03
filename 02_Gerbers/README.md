# Gerbers & Drill Files

Fabrication data for the Doomsday Comms System (D.C.S., SKU OS2601), board
rev B. Generated from KiCad 7. Internal CAD name: **Messager**.

## Board stackup — 4 copper layers

| File | Layer |
|------|-------|
| `Messager-F_Cu.gbr` | Top copper |
| `Messager-In1_Cu.gbr` | Inner copper 1 |
| `Messager-In2_Cu.gbr` | Inner copper 2 |
| `Messager-B_Cu.gbr` | Bottom copper |
| `Messager-F_Mask.gbr` / `Messager-B_Mask.gbr` | Solder mask (top / bottom) |
| `Messager-F_Paste.gbr` / `Messager-B_Paste.gbr` | Solder paste stencil (top / bottom) |
| `Messager-F_Silkscreen.gbr` / `Messager-B_Silkscreen.gbr` | Silkscreen (top / bottom) |
| `Messager-Edge_Cuts.gbr` | Board outline |
| `Messager-PTH.drl` | Plated through-holes (Excellon) |
| `Messager-NPTH.drl` | Non-plated holes (Excellon) |
| `Messager-job.gbrjob` | Gerber job file (manifest tying the set together) |

## Formats

- Gerbers: **RS-274X**.
- Drills: **Excellon**.

## Fabrication

A ready-to-upload archive is provided:

- **`OS2601_DCS_RevB_Gerbers.zip`** — all 14 fabrication files, flat (no
  subfolders), ready to drop into a fab's upload form.

Upload it to your PCB fabricator (JLCPCB, PCBWay, Aisler, etc.) and specify a
**4-layer** board. Confirm finished thickness, copper weight, and surface finish
with the vendor per your requirements. (The loose files are also left in this
folder if you prefer to inspect or re-zip them.)

> The gerber/drill filenames keep the internal design name `Messager-*` — only
> the KiCad project and firmware were renamed to `dcs`/OS2601. This is the same
> board; the names are irrelevant to fabrication. Keep them as-is —
> `Messager-job.gbrjob` references the other files by name, so renaming
> individual gerbers would break the job manifest.
