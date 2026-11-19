# base/system — open work

Scope: `src/base/system`. The current design is `.ai/design/base-system.md`; the live facts and traps
are in `.ai/memory/base-system.md`.

## B5 — macOS physical disks (IOKit)

`Hardware::disks()` reports mounted volumes on macOS (`getmntinfo`), so a whole disk that carries
several volumes appears several times and no entry has a model or a serial. Enumerating `IOMedia`
through IOKit would answer the whole-device question, at the cost of linking IOKit and a registry
walk this half cannot be tested for on the Linux build machine.

Trigger: the first macOS consumer that needs a disk model/serial or a whole-disk capacity.

