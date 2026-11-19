# base/system — open work

Scope: `src/base/system`. The current design is `.ai/design/base-system.md`; the live facts and traps
are in `.ai/memory/base-system.md`.

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
