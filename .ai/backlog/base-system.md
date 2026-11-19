# base/system — open work

Scope: `src/base/system`. The current design is `.ai/design/base-system.md`; the live facts and traps
are in `.ai/memory/base-system.md`.

## B5 — macOS physical disks (IOKit)

`Hardware::disks()` reports mounted volumes on macOS (`getmntinfo`), so a whole disk that carries
several volumes appears several times and no entry has a model or a serial. Enumerating `IOMedia`
through IOKit would answer the whole-device question, at the cost of linking IOKit and a registry
walk this half cannot be tested for on the Linux build machine.

Trigger: the first macOS consumer that needs a disk model/serial or a whole-disk capacity.

## B4 — an OS version and a kernel version are two facts

`OperatingSystemInfo::version` reports the Windows build on Windows but the `uname` kernel release on
Linux and macOS, so an About panel shows "6.8.0-45-generic" where a user expects a distribution
version. The split (`os_version` from `/etc/os-release` / `kern.osproductversion` / the Windows build,
`kernel_version` from `uname`) changes the struct and the per-platform contract, so it is its own
batch rather than a rider on `B2`.

Trigger: the first consumer that displays the version to a user, or the next ABI batch if that comes
first.

