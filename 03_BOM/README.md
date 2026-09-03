# Bill of Materials

Production BOM for the Doomsday Comms System (D.C.S., SKU OS2601), board rev B.

## File

- `OS2601_DCS_RevB_BOM.xlsx` — 27 line items, 44 components.

## Columns

- **Placed** — a checkbox column for hand-assembly / line tracking.
- **Designators** — reference designators (e.g. `C1–C4`, `U2`).
- **Value** — component value.
- **Package** — footprint / package (0805, 0603, SOT-23, MSOP-10, …).
- **Qty** — quantity per board.
- **MPN** — manufacturer part number.
- **Notes** — includes **LCSC part numbers** (e.g. `C354262`) for direct ordering,
  plus alternates where called out.

The list is grouped for assembly convenience (SMD passives first, then ICs and
transistor, etc.).

## Sourcing note

LCSC part numbers are given for most parts for convenient ordering / JLCPCB
assembly. A few parts (e.g. the 10 µF `CL21B106KOQNNNE`) have no LCSC number and
should be ordered by **MPN**. Always verify availability and package/value before
placing an order.

> This is the **production** BOM. An internal costing BOM exists but is
> intentionally **not** included in this package.
