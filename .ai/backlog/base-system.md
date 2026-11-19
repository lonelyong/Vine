# base/system — open work

Scope: `src/base/system`. The current design is `.ai/design/base-system.md`; the live facts and traps
are in `.ai/memory/base-system.md`.

## B3 — macOS gaps the contract currently admits

- `Process::findByName` is not implemented (no `KERN_PROC` enumeration): the empty list is honest but
  a caller cannot tell "no such process" from "not implemented".
- `Hardware::disks` is not implemented (IOKit): same shape of answer.

Trigger: the first macOS consumer of either call.

## B4 — an OS version and a kernel version are two facts

`OperatingSystemInfo::version` reports the Windows build on Windows but the `uname` kernel release on
Linux and macOS, so an About panel shows "6.8.0-45-generic" where a user expects a distribution
version. The split (`os_version` from `/etc/os-release` / `kern.osproductversion` / the Windows build,
`kernel_version` from `uname`) changes the struct and the per-platform contract, so it is its own
batch rather than a rider on `B2`.

Trigger: the first consumer that displays the version to a user, or the next ABI batch if that comes
first.

