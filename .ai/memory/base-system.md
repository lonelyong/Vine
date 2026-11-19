# base/system — current facts

Design: `.ai/design/base-system.md` (contracts, design points, per-platform vocabulary, rejected
options). This page holds only the facts and traps that page does not repeat.

## Layout: one platform, one file

`src/base/system/src/` holds a shared policy half plus one implementation file per platform family:

| File | Holds |
|---|---|
| `SystemPlatform.hpp` | The private seam: `detail::` hooks only. Not installed, not exported. |
| `Process.cpp`, `Hardware.cpp`, `OperatingSystem.cpp`, `Environment.cpp`, `Directories.cpp`, `Host.cpp` | The policy that is the same everywhere: the pid guard, query-once-and-cache, the "never report zero cores" fallback, the "unknown" fill-in, the empty-name guard, the "no trailing separator" rule and the executable-directory derivation. |
| `SystemWindows.cpp` | Every hook for Windows. |
| `SystemLinux.cpp` | Every hook for Linux and Android. |
| `SystemApple.cpp` | Every hook for macOS; disk and process enumeration answer empty by contract. |
| `SystemUnsupported.cpp` | Every hook for any other platform, with the documented empty answers. |

`vn_add_library` globs every `src/**/*.cpp`, so `CMakeLists.txt` marks the three foreign halves
`HEADER_FILE_ONLY` (same arrangement as `base/crash`'s `CrashCapture*.cpp`). Adding a platform is one
new file plus one line in that list. Adding a source file needs a reconfigure: the glob has no
`CONFIGURE_DEPENDS`.

## What the module answers

Static queries only; no handles, no events, no lifetime. `Process`, `OperatingSystem`, `Hardware`,
`Environment`, `Directories`, `Host`. Deliberately absent: process spawning, network interfaces,
display/DPI (`window`), power events, and any registry/driver enumeration - each would turn a query
set into an OS abstraction layer.

## Traps

- clang-format sorts includes and moves `<tlhelp32.h>`, `<winioctl.h>` and `<winternl.h>` **before**
  `<windows.h>`; keep them in a `// clang-format off` block or the Windows half stops compiling.
- Windows reports a variable that exists and is *empty* with a required size of 1 and stores 0
  characters on the second call: existence is decided by the first call's return value alone.
- `GetEnvironmentStringsW` also returns the per-drive working directories as `=C:=C:\dir`; they are
  not name/value pairs and are skipped.
- A dylib on macOS reads the environment array through `_NSGetEnviron()`, not `::environ`.
- An `XDG_*` value that is not absolute is invalid and must be ignored (specification), so
  `~/.local/share` and friends are the fallback, not `env + suffix`.
- `getpwuid_r`, never `getpwuid`: the latter answers from one shared static buffer.
- `availableBytes` is a snapshot that moves with every allocation on the machine - never cache it,
  and do not read `MemoryInfo` as "memory free for this process".
- `/sys/block` lists whole devices only; `/sys/class/block` also lists partitions. Never decide "is a
  partition" from the name: a "contains a digit" test drops `nvme0n1` and `mmcblk0`, i.e. every disk
  of a modern machine.
- `/sys/block/<dev>/size` is a count of 512-byte sectors for every device, whatever its logical block
  size is.
- sysfs pads fixed-width fields with spaces (`"Virtual Disk    "`): trim before publishing a model or
  serial.
- `Process::exists()` answers existence, not accessibility: POSIX `EPERM` and Windows
  `ERROR_ACCESS_DENIED` both mean "the process is there".

## Contracts worth not breaking

- `OperatingSystemInfo::version` is each platform's own spelling: Windows `major.minor.build`;
  Linux and macOS the `uname` kernel release.
- `DiskInfo` is a volume (with the backing disk's identity) on Windows and a whole block device on
  Linux; `Hardware.hpp` states both.
- `kind` is `DiskKind::CdRom` for an optical drive, whose capacity is 0 while it holds no media; the
  five-value enum states its width (`std::uint8_t`), and the core counts and byte counts are
  `std::uint32_t` / `std::uint64_t`.
- `Directories` answers the *roots* only; `<root>/<organization>/<application>` is appfw's layout.
  No path carries a trailing separator, and nothing here creates a directory.
- `Environment` is read-only on purpose: the environment is process-global state shared with every
  dependency, so this module never sets or clears a variable.
- Failures are silent by design: an empty string, an empty list or 0 - never an exception, never a
  guess.
- The Windows and macOS halves are not compiled on Linux; a `-fsyntax-only -Wall -Wextra
  -Wconversion` pass covers only the shared and Linux halves, so a Windows-only edit must be read,
  not trusted to the build.
