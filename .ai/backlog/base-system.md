# base/system — open work

Scope: `src/base/system`. Current layout and traps are in `.ai/memory/base-system.md`; the design page
does not exist yet (see B1).

## B1 — design page for a platform-seam module

`base/system` is the only platform-dependent `base` module without an `.ai/design/` page
(`core`, `crypto`, `runtime`, `crash`, `progress`, `iobase`, `window` have one). The page has to state
the per-platform semantics that today live only in the SDK headers: `version` spelling, `exists()`
permission answer, `DiskInfo` volume-vs-device, and the empty answers of macOS / unsupported platforms.

Trigger: the next contract change in this module, or the first product use of `Hardware`/`Process`.
Doing it earlier is imagination; doing it later means the page is written from memory.

## B2 — value types that the SDK rule already describes

The repository rule says identifiers, counts and bit masks use a fixed-width unsigned type, and that a
public value enum states its underlying type. `base/system` predates the rule and is the only module
still spelling it otherwise:

- `CpuInfo::physicalCores` / `logicalCores`: `unsigned` -> `std::uint32_t`.
- `DiskInfo::capacity`, `Hardware::diskTotalSpace` / `diskFreeSpace`: `unsigned long long` ->
  `std::uint64_t`.
- `DiskInfo::kind`: `String` -> `enum class DiskKind : std::uint8_t`.

All three break the ABI. Trigger: the first batch that already breaks this module's ABI (as the enum
rule in `.github/copilot-instructions.md` requires), not a comment sweep.

## B3 — macOS gaps the contract currently admits

- `Process::findByName` is not implemented (no `KERN_PROC` enumeration): the empty list is honest but
  a caller cannot tell "no such process" from "not implemented".
- `Hardware::disks` is not implemented (IOKit): same shape of answer.

Trigger: the first macOS consumer of either call.
