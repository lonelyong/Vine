# base/system

> **Status**: living
> **Last verified**: 2026-11-19
> **Scope**: `src/base/system` — the platform query module: `Process`, `OperatingSystem`, `Hardware`,
> `Environment`, `Directories`, `Host` and the private seam they are built on. It does not cover
> process creation, windowing and display geometry, networking, power events or device I/O.
> **Sources**: `sdk/vine/system/*.hpp`, `src/SystemPlatform.hpp`, `src/System*.cpp`,
> `tests/test_system/SystemTest.cpp`.
> **Backlog**: `../backlog/base-system.md`

## 1. Overview

### 1.1 Problem and Background

Every layer above `base` needs facts about the machine it runs on: the operating system and its
architecture (an About panel, a log header), the CPU and memory (a default worker count, a cache
limit), the volumes (a disk-space warning), the environment and the per-user directory roots (where
to persist a configuration), the process identity (a single-instance guard, a crash report) and the
host and user names (a support ticket).

None of these are policy. They are queries the platform answers, and the only interesting questions
are: which spelling does each platform report, and what does a caller get when the platform cannot
answer.

The module's first implementation answered them from three files in which each platform was a
`#ifdef` branch inside each function. A branch that the current build never compiles is a branch
nobody has compiled: that arrangement shipped a non-void function with no return path on a platform
outside the three it knew, `__cpuid` on an ARM64 Windows build, and a disk-name heuristic that
dropped every NVMe device (`../memory/base-system.md` records the details).

### 1.2 Goals

- One place answers "what does this platform report", and the answer's spelling is stated in the
  header rather than discovered by experiment.
- A platform half is compiled as a whole on the platform that owns it; a missing function is a link
  error there, not a silently dead branch elsewhere.
- Every query has a defined answer when the platform cannot answer: an empty string, an empty list or
  0, and a header that says so.
- Adding a platform, or a new query, does not touch the core model: one new file, or one new hook
  implemented in each half.

### 1.3 Non-goals

- Process creation. A general spawn needs pipes, timeouts and exit codes, which is its own design;
  the crash handler's `fork`/`CreateProcessW` path is that module's privileged exception.
- Window, display, DPI and session state. Those belong to `base/window` and the graphics backends.
- Network interfaces, power and sleep events, registry or driver enumeration, and anything that hands
  out a handle or needs an event loop. Each would turn a set of static queries into an OS abstraction
  layer, and none has a consumer today (`G8`).
- Mutating the environment. The environment is process-global state shared with every dependency.

### 1.4 One-paragraph Conclusion

`base/system` is a set of static queries over one private seam. Each public class holds the policy
that is the same everywhere (the pid guard, "query once and cache" where the fact is a constant,
"never report zero cores", the empty-name guard, the "no trailing separator" rule, the `unknown`
fill-in); each platform family implements the mechanism in its own file behind `SystemPlatform.hpp`.
No query throws, no query reports a reason, and no query owns a handle - which is what keeps the
module small enough that a platform half can be read in one pass.

## 2. Design Points

### 2.1 **A platform capability is a whole file, not a function-level `#ifdef`** (G1, G9, G11)

- What it is: `SystemWindows.cpp`, `SystemLinux.cpp`, `SystemApple.cpp` and `SystemUnsupported.cpp`
  each answer every hook of the seam for their platform; exactly one of them is compiled.
- Why: the deciding property is that the platform's code is compiled there. A branch inside a
  function is only compiled if the build happens to be that platform, so the other branches rot
  without a compiler ever reading them (three such defects are in §1.1).
- Boundary: the split is per platform family, not per function or per class, because the module's
  classes share one seam; splitting further would add files without adding a check.

### 2.2 **The seam holds the mechanism, the public class holds the policy** (G2, G7)

- What it is: `SystemPlatform.hpp` declares free functions in `vn::system::detail`
  (`fillOsInfo`, `enumerateDisks`, `environmentVariable`, `temporaryDirectory`, ...). The public
  classes validate, normalise, cache and document; the platform halves only ask the operating system.
- Why: the same rules would otherwise be written four times, once per platform file, and the fourth
  copy would be the one that forgot the guard.
- Boundary: the seam is private - not installed, not exported; it is not an extension point.

### 2.3 **Silence, not error codes** (G4, G8)

- What it is: a query that cannot answer returns its empty value; nothing returns an error code, a
  reason string or an `expected`.
- Why: every consumer reads the fact for display or for a default. A reason nobody acts on is cost on
  every call and on every signature. The header states the empty value, so "no answer" is part of the
  contract instead of an accident.
- Boundary: an error vocabulary returns with the first consumer that acts on a failure (a spawn that
  must report why it failed, say); it does not arrive before that.

### 2.4 **The platform vocabulary is part of the contract** (G4, G6)

- What it is: `OperatingSystemInfo` separates the product version from the kernel release -
  `osVersion` is what a user reads (Windows `major.minor.build`, Linux the distribution's
  `PRETTY_NAME`, macOS `kern.osproductversion`) and `kernelVersion` is the `uname` release the
  process actually runs on; `DiskInfo` is a volume on Windows
  and on macOS, and a whole block device on Linux; `kind` is `DiskKind`, a closed set of five values
  with a stated width; `exists()` answers existence, not accessibility.
- Why: an About panel that prints "6.8.0-45-generic" where the user expects a distribution version
  is showing the wrong fact, and a container that runs another kernel than its product version
  suggests makes the two genuinely different.
- Boundary: a field stays a string where the platform's vocabulary is open (architecture, model); a
  closed end of the vocabulary is an `enum class` with a stated underlying type (`DiskKind`), so a
  caller cannot invent a sixth word and a log can print one.

### 2.5 **Cache only what cannot change** (G10)

- What it is: `OperatingSystem::info()`, `Hardware::cpu()` and `Hardware::motherboard()` are built
  once in a function-local static. `disks()`, `memory()`, `Environment` and `Directories` answer on
  every call.
- Why: a cached `availableBytes` publishes a stale fact as if it were current, and a cached directory
  root ignores a host that replaced the environment after startup. A function-local static is
  initialised under the language's guard variable, so the cached ones are thread-safe without a lock.
- Boundary: "cheap" is not the criterion - `Directories` is cheap and still not cached; invariance is.

### 2.6 **No handle, no lifetime, no callback** (G1, G8)

- What it is: every class is a namespace of `static` functions returning values; the module owns
  nothing across a call and schedules nothing.
- Why: a caller can use the module from any thread, in any phase of startup, including inside a crash
  path, without a lifecycle to respect. `base/crash` in fact reads the environment directly for that
  reason.
- Boundary: this is the line that keeps power events, device notifications and session changes out
  (§1.3, `D5`).

## 3. Features

### 3.1 Feature Index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Process queries | Current pid and executable path, existence, lookup by executable name, forceful termination | landed |
| F2 | Operating system facts | Name, product version, kernel release, architecture; built once and cached | landed |
| F3 | Hardware facts | CPU, motherboard, memory snapshot, storage enumeration, volume capacity and free space | landed |
| F4 | Environment | Presence, value, full enumeration; read-only | landed |
| F5 | Directory roots | Temporary, home, data, config, cache, executable directory | landed |
| F6 | Host identity | Machine name and account name | landed |

### 3.1.1 F1 Process queries

- **Behaviour**: `currentProcessId()` and `currentExecutablePath()` report the calling process;
  `exists(pid)` answers whether a process with that id is there; `findByName(name)` returns the ids
  whose executable file name equals `name`; `kill(pid)` terminates it.
- **Constraints** (C1, C5): identifiers <= 0 and the empty name are rejected in `Process.cpp`, once
  for every platform.
- **Requirements** (R1, R2): Windows compares executable names case-insensitively, Linux exactly;
  `exists()` reports existence even when the process cannot be opened.
- **Acceptance** (T1).
- **Caveats** (N1): a process the caller may not inspect is skipped on both POSIX halves (a refused
  `/proc/<pid>/exe` read, a refused `proc_pidpath`); Linux does not match a process whose executable
  was deleted (" (deleted)" suffix).

### 3.1.2 F2 Operating system facts

- **Behaviour**: `OperatingSystem::info()` returns `OperatingSystemInfo` with `name` (`Windows`,
  `Linux`, `macOS`), `osVersion`, `kernelVersion` and `architecture`; built once on first call and
  cached.
- **Constraints** (C2): a fact the platform cannot answer reads `unknown`, never an empty string.
- **Requirements** (R3): each version field states the fact it names (§2.4).
- **Acceptance** (T2).
- **Caveats** (N2): Windows has one version, so both fields report the OS build there; a Linux
  distribution that publishes no `os-release` answers `unknown`.

### 3.1.3 F3 Hardware facts

- **Behaviour**: `cpu()` and `motherboard()` are cached; `memory()` answers a snapshot
  (`MemoryInfo`); `disks()` enumerates the platform's storage; `diskTotalSpace(path)` and
  `diskFreeSpace(path)` answer for the volume holding a path.
- **Constraints** (C3, C4): core counts are never 0; `disks()` reports whole devices only and never a
  partition; a `DiskKind::CdRom` entry may report capacity 0.
- **Requirements** (R4, R5): `availableBytes` is what the system can hand to a new allocation;
  `diskFreeSpace` is space available to the calling user.
- **Acceptance** (T3).
- **Caveats** (N3): macOS reports mounted volumes (no model, no serial); Windows reports volumes
  with the backing disk's identity while Linux reports block devices.

### 3.1.4 F4 Environment

- **Behaviour**: `has(name)`, `value(name)` and `variables()` read the inherited environment.
- **Constraints** (C6): read-only; empty names never match; names and values are UTF-8 on every
  platform.
- **Requirements** (R6): `has()` distinguishes "set to empty" from "absent".
- **Acceptance** (T4).
- **Caveats** (N4): Windows hides the per-drive `=C:=...` pseudo entries.

### 3.1.5 F5 Directory roots

- **Behaviour**: `temporary()`, `home()`, `data()`, `config()`, `cache()` and
  `executableDirectory()`.
- **Constraints** (C7): roots only; no trailing separator; nothing is created.
- **Requirements** (R7): `XDG_*` wins when it is absolute; the per-application layout stays the
  framework's (appfw).
- **Acceptance** (T5).
- **Caveats** (N5): an empty answer is legitimate - a container can run without a home.

### 3.1.6 F6 Host identity

- **Behaviour**: `hostName()` and `userName()`.
- **Constraints** (C8): read on every call; an empty string when the platform cannot answer.
- **Requirements** (R8): the account name, not the display name.
- **Acceptance** (T6).
- **Caveats** (N6): display identities, not authentication inputs.

## 4. Public Interface

### 4.1 Process

| Interface | Contract | Notes |
|---|---|---|
| `Process` | Static process queries; no process is spawned | `Process.hpp` |
| `currentProcessId()` | The calling process' id | |
| `currentExecutablePath()` | Absolute path, or empty | empty includes an unimplemented platform |
| `exists(pid)` | `true` for an existing process, including a protected one | |
| `findByName(name)` | Ids whose executable file name equals `name` | `/proc` on Linux, `proc_pidpath` on macOS |
| `kill(pid)` | `true` when the platform reports termination | SIGKILL / `TerminateProcess` |

### 4.2 OperatingSystem

| Interface | Contract | Notes |
|---|---|---|
| `OperatingSystem` | Host OS facts, cached | `OperatingSystem.hpp` |
| `info()` | `OperatingSystemInfo` borrowed for the process lifetime | thread-safe |

### 4.3 Hardware

| Interface | Contract | Notes |
|---|---|---|
| `Hardware` | CPU, motherboard, memory, storage | `Hardware.hpp` |
| `cpu()` | `CpuInfo`, cached | never reports 0 cores |
| `motherboard()` | `MotherboardInfo`, cached | |
| `memory()` | `MemoryInfo` snapshot | not cached |
| `disks()` | `std::vector<DiskInfo>`; `kind` is a `DiskKind` | not cached; real I/O |
| `diskTotalSpace(path)` | Bytes, 0 when unknown | |
| `diskFreeSpace(path)` | Bytes available to the caller, 0 when unknown | |

### 4.4 Environment

| Interface | Contract | Notes |
|---|---|---|
| `Environment` | Read-only environment access | `Environment.hpp` |
| `has(name)` | `true` when set, including set to empty | |
| `value(name)` | The value, empty when unset | |
| `variables()` | Every name/value pair | platform order |

### 4.5 Directories

| Interface | Contract | Notes |
|---|---|---|
| `Directories` | Per-user roots | `Directories.hpp` |
| `temporary()` | System temporary root | |
| `home()` | The user's home | |
| `data()` | Per-user application data root | |
| `config()` | Per-user configuration root | |
| `cache()` | Per-user cache root | equals `data()` on Windows |
| `executableDirectory()` | Directory of the running executable | derived from `currentExecutablePath()` |

### 4.6 Host

| Interface | Contract | Notes |
|---|---|---|
| `Host` | Machine and account identity | `Host.hpp` |
| `hostName()` | DNS host name where the platform has one | |
| `userName()` | Account name | |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints

- **C1** An identifier <= 0 and an empty executable name are rejected once, in the shared half.
- **C2** `name`, `osVersion`, `kernelVersion` and `architecture` are never empty; an unknown fact
  reads `unknown`.
- **C3** `CpuInfo::logicalCores` and `physicalCores` are at least 1 and `physicalCores <=
  logicalCores` after the shared fallback.
- **C4** `disks()` returns whole devices only; a partition is never reported.
- **C9** A public value enum states its underlying type: `DiskKind : std::uint8_t`; the value counts
  and byte counts are `std::uint32_t` / `std::uint64_t`, never `unsigned` or `unsigned long long`.
- **C5** A query never throws and never reports a reason; its empty value is the whole answer.
- **C6** `Environment` never writes; names and values cross the seam as UTF-8.
- **C7** A `Directories` answer carries no trailing separator and never creates anything.
- **C8** `Host` answers on every call and is never cached.

### 5.2 Requirements

- **R1** `exists()` answers existence, not accessibility: POSIX `EPERM` and Windows
  `ERROR_ACCESS_DENIED` both mean "there".
- **R2** `findByName()` compares file names: case-insensitive on Windows, exact on Linux.
- **R3** `osVersion` is the product version and `kernelVersion` is the kernel release; neither
  substitutes for the other (§2.4).
- **R4** `MemoryInfo::availableBytes` is what a new allocation can claim; `totalBytes` is physical
  memory.
- **R5** `diskFreeSpace()` is available-to-the-caller, not unused (`f_bavail`, not `f_bfree`).
- **R6** `has()` tells an empty value from an absent variable.
- **R7** An `XDG_*` value is used only when absolute; the fallback is the specification's directory.
- **R8** `userName()` is the account name, not a display name.

### 5.3 Invariants

- **N1** Every platform half answers every hook of the seam; the unsupported half answers all of
  them with the documented empty values.
- **N2** Only facts that cannot change are cached; a snapshot query is never cached.
- **N3** `Directories::executableDirectory()` and `Process::currentExecutablePath()` come from the
  same call, so one cannot disagree with the other.
- **N4** A Windows existing-but-empty variable is reported as existing, never as absent.

## 6. Caveats and Traps

### 6.1 Windows include order

- How it fails: clang-format sorts includes, moves `<tlhelp32.h>`, `<winioctl.h>` and `<winternl.h>`
  above `<windows.h>`, and the Windows half stops compiling.
- How to detect it: the Windows build fails in `SystemWindows.cpp`, which cannot be built on Linux.
- How to prevent it: the three headers stay in the `// clang-format off` block.

### 6.2 Whole devices versus partitions

- How it fails: deriving "is a partition" from a name (for example "contains a digit") also drops
  `nvme0n1` and `mmcblk0`, i.e. every disk on a modern machine.
- How to detect it: `EveryKnownWholeDeviceIsReported` compares the answer with `/sys/block`.
- How to prevent it: enumerate `/sys/block`, which contains whole devices only.

### 6.3 Windows variables that exist and are empty

- How it fails: `GetEnvironmentVariableW` reports a required size of 1 for such a variable and stores
  0 characters on the second call, so a "copied == 0" test reads it as absent.
- How to detect it: `has()` on a variable the process just set to `""`.
- How to prevent it: decide existence from the first call's answer alone (`N4`).

### 6.4 Environment array in a shared library on macOS

- How it fails: `::environ` from a dylib is not guaranteed to be the process' array.
- How to detect it: `variables()` returns few or no entries on macOS.
- How to prevent it: read `_NSGetEnviron()`.

### 6.5 Relative XDG paths

- How it fails: a relative `XDG_DATA_HOME` would be joined to the current directory at every use,
  i.e. the answer changes with the working directory.
- How to detect it: `data()` differs between two calls from different directories.
- How to prevent it: ignore a non-absolute value (`R7`).

### 6.6 `getpwuid` and shared state

- How it fails: `getpwuid` answers from one static buffer; two threads calling `userName()` can read
  each other's entry.
- How to detect it: mismatched names under TSan with concurrent callers.
- How to prevent it: `getpwuid_r` into a local buffer.

## 7. Rejected Options

| # | Option | Why not | ADR |
|---|---|---|---|
| D1 | One `.cpp` per platform with a `#ifdef` inside each function | The branches the build does not take are never compiled, which is how the missing return and the ARM64 `__cpuid` survived; violates `G11` | none |
| D2 | An abstract `ISystemBackend` with virtual dispatch | One implementation exists per build, so the indirection buys nothing and adds a lifetime (`G7`, `G8`) | none |
| D3 | Error codes or `expected` on every query | Every consumer reads a display fact; a reason nobody acts on costs every signature (`G8`) | none |
| D4 | Caching every answer | `availableBytes`, volume lists and directory roots change while the process runs; a cache would publish stale facts (`G10`) | none |
| D5 | Adding process creation, power events or device notifications | Each needs pipes/timeouts or an event loop, i.e. a different kind of module (`G1`, `G8`) | none |

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
|---|---|---|
| T1 | Process queries answer for the calling process and reject bad input | `CurrentProcessIdIsPositive`, `ExistsForCurrentProcess`, `FindByNameFindsCurrentProcess`, `KillMissingProcessFails`, `FindByNameWithAnEmptyOrUnknownNameFindsNothing` |
| T2 | OS facts are filled and cached | `OperatingSystemInfoIsFilled`, `InfoIsCached` |
| T3 | Hardware facts are filled, cached where promised, and storage is whole devices | `CpuInfoIsFilled`, `CpuInfoIsCached`, `MemoryInfoIsFilled`, `DisksAreEnumerated`, `ReportedDisksAreWholeDevices`, `EveryKnownWholeDeviceIsReported`, `DiskSpaceForCurrentPath`, `DiskSpaceOfAnUnknownPathIsZero` |
| T4 | Environment presence, value, round trip and enumeration | `EnvironmentAnswersPresenceAndValue`, `EnvironmentSeesAVariableSetByTheProcess`, `EnvironmentListsEveryVariable` |
| T5 | Directory roots are absolute-shaped, exist where promised, and contain the executable | `DirectoryRootsHaveNoTrailingSeparator`, `TemporaryAndHomeAreRealDirectories`, `ExecutableDirectoryContainsTheExecutable` |
| T6 | Host identity is answered | `HostIsIdentified` |

Command: `ctest -R test_system`. The motherboard case skips where `/sys/class/dmi/id` is not exposed
(WSL, containers), because the platform has no answer there.

## 9. Status and Evolution

### 9.1 Current Status

All six features are landed and covered by `tests/test_system` (25 cases). The Windows and macOS
halves are not compiled on the Linux build machine; every edit to them is reviewed by reading, and a
`-fsyntax-only -Wall -Wextra -Wconversion` pass covers only the shared and Linux halves.

### 9.2 Known Gaps and Backlog

- `B5`: macOS reports mounted volumes, not physical disks; whole-disk capacity, model and serial need
  IOKit's `IOMedia` registry, which this half does not link.

### 9.3 Compatibility and Migration

The ABI batch (`B2`) broke three declarations on purpose; a consumer must recompile, and no source
change is needed:

- `CpuInfo::physicalCores` / `logicalCores`: `unsigned` -> `std::uint32_t`.
- `DiskInfo::capacity`, `Hardware::diskTotalSpace()` / `diskFreeSpace()`: `unsigned long long` ->
  `std::uint64_t`.
- `DiskInfo::kind`: `String` -> `DiskKind`; a caller that compared words now compares
  `DiskKind::CdRom` and friends.

The version split (`B4`) changed the same struct again: `OperatingSystemInfo::version` is gone, and
`osVersion` / `kernelVersion` take its place with the two facts it used to conflate. A consumer that
printed `version` now chooses which of the two it means - an About panel takes `osVersion`.

The class set stays additive otherwise: `Environment`, `Directories` and `Host` are separate classes
and `memory()` extends `Hardware` without changing existing signatures. A platform the module does
not implement answers with defined empty values instead of undefined behaviour, so a port starts by
adding one file.

## 10. References

### 10.1 Code

- `src/base/system/sdk/vine/system/Process.hpp`, `OperatingSystem.hpp`, `Hardware.hpp`,
  `Environment.hpp`, `Directories.hpp`, `Host.hpp` — the contracts.
- `src/base/system/src/SystemPlatform.hpp` — the private seam.
- `src/base/system/src/SystemWindows.cpp`, `SystemLinux.cpp`, `SystemApple.cpp`,
  `SystemUnsupported.cpp` — the mechanism, one platform family per file.
- `src/base/system/src/Process.cpp`, `OperatingSystem.cpp`, `Hardware.cpp`, `Environment.cpp`,
  `Directories.cpp`, `Host.cpp` — the shared policy.
- `tests/test_system/SystemTest.cpp` — the acceptance cases.

### 10.2 Documents and ADRs

- `../memory/base-system.md` — live facts and traps (`/sys/block`, sysfs padding, the Windows
  environment quirk).
- `../backlog/base-system.md` — the open items named in §9.2.
- `base-crash.md` — the precedent for a private seam with one file per platform
  (`src/base/crash/src/CrashDetail.hpp`).

### 10.3 External Sources

- `GetEnvironmentVariableW`, `GetEnvironmentStringsW`, `GlobalMemoryStatusEx`,
  `GetLogicalProcessorInformationEx`, `GetTempPathW`, `GetComputerNameExW` — Win32 API reference.
- `proc(5)`, `sysfs` block-device attributes, `sysinfo(2)`, `getpwuid_r(3)`, the `os-release`
  specification - Linux manual pages.
- XDG Base Directory Specification.
- `host_statistics64`, `sysctlbyname`, `_NSGetExecutablePath`, `_NSGetEnviron`, `proc_listpids`,
  `proc_pidpath`, `getmntinfo` — macOS API reference.
