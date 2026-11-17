# crash reports (`vn::Crash` and `CrashHandler.exe`)

> **Status**: current
> **Last verified**: 2026-10-30
> **Scope**: in-process crash capture (Windows / Linux), the report text format, and `CrashHandler`'s symbolization and window. Not upload, aggregation or a cross-platform dump format; Linux produces no dump.
> **Sources**: `src/base/crash/sdk/vine/crash/CrashHandler.hpp` (the contract), `tests/test_crash/` (the behaviour).
> **Backlog**: `../backlog/base-crash.md`

## 1. Overview

### 1.1 Problem and background

A hand-written Windows SEH filter in `main.cpp` (about 160 lines) walked the stack with DbgHelp inside the main process, wrote an unsymbolized `vn_crash.log` and then exited.
That has three problems: it touches DbgHelp and the heap on the crash path (dangerous in itself), it leaves no dump (a debugger gets no scene), and the user sees no window and does not know where the file is.

### 1.2 Goals

- One crash leaves three things behind: a **report** (text, parseable), a **dump** (for a debugger) and a **window** (telling the user what to do).
- The crash path allocates nothing, takes no lock, throws nothing, and does not depend on a library that may itself be at the crash site.
- The report format is stable: a field written first stays written first, and new fields are only appended.
- The module belongs to the `base` layer: no Qt, no appfw, usable by any process (tools included).

### 1.3 Non-goals

- No upload / aggregation / upload queue (there is no server and no agreed support channel).
- No cross-platform dump format of its own and no symbol server.
- No UI inside the crashing process (the heap may be broken, and Qt may be the crash site).

### 1.4 One-paragraph conclusion

`vn::Crash` installs a last-chance hook while the process is healthy (Windows SEH / Linux sigaction); on a crash it finishes the report in static buffers, writes the dump (Windows only), starts the `CrashHandler` beside it and exits.
The handler is a separate healthy process: read the report → symbolize with the platform tool (Windows DbgHelp / Linux `addr2line`) → write the human-readable `.txt` → show the window (Windows TaskDialog / Linux Xlib + Xft).

## 2. Design Points

### 2.1 Placement: `src/base/crash` (G2, G12)

- What it is: the `vn::Crash` library plus the `CrashHandler` executable target beside it.
- Why: crash capture is a platform fact, not a framework capability; putting it in appfw would make a low-level need depend on a high-level one, and `src/fw` would mislead the dependency direction.
- Boundary: the library depends only on `vn::Global`, `vn::Core` and, on Windows, privately links `dbghelp`. The handler links only `vn::Crash` + **system libraries**, never appfw and never a UI toolkit: Windows uses the system TaskDialog, Linux draws a window with Xlib and renders text with Xft (`../decisions/ADR-2026-10-30-crash-handler-x11-on-linux.md`).

### 2.2 The report file is the only interface between the processes (G4, G5)

- What it is: the crashing process writes a file, starts a process and dies; the handler reads that one file (plus the dump path).
- Why: no IPC protocol, no resident process, no orphan reaping; the format is the contract and can be unit-tested.
- Boundary: with a field missing the handler degrades its display; it does not reject the whole file.

### 2.3 The crash path allocates nothing, takes no lock, throws nothing (G10)

- What it is: the report's image table / frames / context are assembled in **static** buffers (not on an already overflowed stack), the text renders into a static `char[32768]`, and the file is written with `WriteFile`.
- Why: the state of the heap and of the CRT locks is unknown during a crash; the logging module may hold a lock, so it is not used.
- Boundary: truncation is an allowed degradation (see 3.2), but it is not “drop a few lines and carry on” — rendering stops at the first line that does not fit.

### 2.4 Symbolization moves into the handler (G7, G11)

- What it is: the crashing process records `module + offset` only; names and source lines are resolved by the handler in a healthy process — DbgHelp on Windows, an external `addr2line` on Linux.
- Why: symbol resolution is heavy work, slow and dangerous inside the crashing process, while the handler may allocate freely, start external processes and look at symbol paths.
- Boundary: with no symbols a frame degrades to `module + offset`, and a frame is never dropped.

### 2.5 Symbols need the image's preferred base (Windows, G11)

- What it is: the handler reads `ImageBase` and `SizeOfImage` from the PE header, loads the image at **its own preferred base**, and queries `preferred base + offset`.
- Why: DbgHelp resolves symbols as `base + RVA`, and that base must be the one the PDB was produced with; the address the crashing process actually mapped changes every boot (ASLR).
  With a base but no size DbgHelp treats the module as 0 bytes, no address falls inside it, and “symbols were never requested” — which looks like “the PDB is missing”.
- Boundary: two images may share a preferred base (`KERNELBASE` and `ntdll` are both `0x180000000`), DbgHelp keys on the base, and a later load displaces an earlier one; on a collision the later one gets an integer multiple of `0x10000000000` added.
  Linux needs none of this: the offset in the report is the link-time virtual address and `addr2line` takes it directly.

### 2.6 The window shows and exports only (G4, G8)

- What it is: the window shows the report / dump paths and expands to the symbolized report; three of its four actions export (copy, open folder, mail draft) and the fourth closes.
- Why: the crash scene is untrusted and the window should not try to “fix” or restart the application; all it can do is put the files in a person's hand.
- Boundary: `mailto:` cannot carry an attachment, so the mail action's text must tell the user to drag the dump in.

### 2.7 The module does not know the product (G1)

- What it is: `Config::app_name` + `Config::context` are the whole product entry point; the report's first line (such as `Vine 1.0.0 (Debug)`) is composed by the application.
- Why: one handler serves every application in `bin/`, and a product name baked in would tie it down.
- Boundary: an empty `app_name` degrades to the exe file name, and then to `app`.

### 2.8 One handler per `bin/` (G10)

- What it is: the handler ships in the same directory as `Vine.exe` and is started by **absolute path**, never through `PATH`.
- Why: one build ⇒ no version drift between writing and reading the report; only a machine-wide shared copy would have that problem.
- Boundary: with the handler missing only the report and the dump are written (a degradation that does not affect the exit).

### 2.9 Linux: a sigaction hook + the `/proc/self/maps` image table (G10, G11)

- What it is: `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE`/`SIGABRT` each get a `SA_SIGINFO | SA_RESETHAND` handler: fill the report → `open`/`write` → `fork` + `execv` the handler → `raise` again, so the process really dies from the signal (exit code 128+n, core still decided by `RLIMIT_CORE`).
- The image table: `/proc/self/maps` is read at crash time, `resolveImageTable` folds the file mappings into one entry per file (base = the lowest mapped address), and a frame is then attributed to an `image` + `offset`.
- Why not `dl_iterate_phdr`/`dladdr`: they take the loader lock, and the crash may happen inside `dlopen`; a snapshot at install time would miss plugins loaded later. `/proc/self/maps` is produced by the kernel at read time.
- Boundary: the timestamp uses the UTC offset computed at install time (never `localtime` on the crash path); anonymous ranges and `[heap]`/`[stack]`/`[vdso]` are not images, so a frame inside one keeps its bare address.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | install and uninstall | `install(Config)` resolves the paths, snapshots the configuration and hooks the last-chance filter (Windows SEH / Linux sigaction); `uninstall()` restores it | landed |
| F2 | unsymbolized report | a crash writes the `vn-crash-report 1` text (context, exception code / address / thread, image table, frames) | landed |
| F3 | dump | a `MiniDumpNormal` dump in the same directory; a failure deletes the half-written file | landed (Windows only) |
| F4 | start the handler | `CreateProcess` (Windows) / `fork` + `execv` (Linux) with `--report/--app/--mailto`, without waiting for it | landed |
| F5 | report rendering / parsing | `renderReport` (pure, writes into a given buffer) and `parseReport` (skips unknown keys) | landed |
| F6 | symbolization | the handler resolves function names and source lines: Windows reads the PE header + DbgHelp, Linux calls `addr2line` | landed |
| F7 | human-readable text | the handler writes `<report name>.txt` (beside the report), one layout for both platforms | landed |
| F8 | crash window | the paths + the report body + copy report / open folder / send mail / close; Windows TaskDialog, Linux hand-drawn Xlib + Xft | landed |
| F9 | symbol diagnostics | a missing symbol is explained at the end of the `.txt` (Windows with the DbgHelp error code, Linux with the addr2line exit code) | landed |
| F10 | image table | `/proc/self/maps` → an `image` table + each frame's `image`/`offset` (pure, unit-testable) | landed (Linux) |

### 3.1.1 F1 install and uninstall

- **Behaviour**: an empty path takes its default (`<exe dir>/<app>_crash.log`, `<app>_crash.dmp`, `CrashHandler.exe`; on Linux the extensionless `CrashHandler`); `handler_exe` is started only when the file exists at install time (`X_OK` on Linux). `install` returns true on both Windows and Linux.
- **Constraints** (C1): only the last-chance filter is hooked, never a first-chance one — an exception caught by `catch` or by a debugger is not a crash.
- **Constraints** (C2): one process has one set of facilities; a repeated `install` replaces rather than stacks.
- **Requirements** (R1): the process behaves the same after `install`; after `uninstall` the filter is no longer called.
- **Acceptance** (T1, T2).
- **Caveats** (N1): a process that decides its own lifetime — a test host, a plugin host — must `uninstall()`, or its report and exit code are replaced.

### 3.2 F2/F5 the report format

```
vn-crash-report 1
app Vine
time 2026-10-30T15:17:42
dump E:\...\Vine_crash.dmp
context Vine 1.0.0 (Debug)
exception 0xc0000005 at 0x00007ff7645f3bbc thread 53560
image 0x00007ff7645f0000 Vine.exe E:\...\Vine.exe
frame 0x00007ff7645f3bbc 0 0x3bbc
```

- **Behaviour**: one `key value...` per line; `image` and `frame` take one line each, the second field of `frame` is the image index (`-1` = not inside any loaded image) and the third is the offset.
- **Constraints** (C3): `vn-crash-report <version>` is the only hard requirement; an unknown version rejects the whole file (half a frame table is worse than none), an unknown key is skipped.
- **Constraints** (C4): rendering **stops** at the first line that does not fit, so the output is always a **prefix** of the complete report — never “a frame pointing at a truncated image table”.
- **Requirements** (R2): `parseReport(renderReport(r)) == r`.
- **Acceptance** (T3, T4).

### 3.3 F6/F7 the handler's symbolization and text

- **Behaviour**: Windows goes `SymInitializeW(search path)` → each image loaded by `ImageBase` + `SizeOfImage` → frame address = `base + offset` → function name + `[file:line]`; Linux groups by image and runs one `addr2line -C -f -e <image> <offset...>`, folding its two lines per frame and treating `??` and `??:0` as unresolved.
- **Constraints** (C5): the symbol search path = the handler's directory + each image's directory + `_NT_SYMBOL_PATH` (the handler inherits the crashing process's environment, so no extra configuration is needed).
- **Requirements** (R3): without a PDB a frame still prints `module + offset` (the offset is what a rebuilt binary is compared against).
- **Acceptance** (T5).

### 3.4 F8 the crash window

- **Behaviour**: Windows uses `TaskDialogIndirect` (comctl32 v6 through a manifest pragma, no `.rc`), with the two paths as the body and the symbolized text in the expansion area; Linux draws its own Xlib window (title `<app> has crashed`, the paths + a scrollable monospaced report, rendered with Xft), and with no display, font or drawing context it creates no window and prints the text to stdout.
- **Copy report**: puts the whole `.txt` plus a trailing `dump: <path>` on the clipboard (what a developer wants is the text that can be pasted straight into an issue).
- **Open folder**: Windows runs `explorer /select,<dump>` (the report's directory when there is no dump); Linux tries `explorer.exe <\\wsl.localhost\…>` (WSL), `xdg-open`, `gio open` and the file managers in turn, each checked for immediate failure; when all fail it puts the directory on the clipboard and says so in the status line — a button must never “do nothing when clicked”.
- **Send mail**: appears only when `--mailto` was given; it opens the default mail client with the recipient / body prefilled and the user must add the attachment.
- **Caveats** (N2): when `TaskDialogIndirect` is unavailable it falls back to `MessageBoxW` (paths only), and when that fails too no dialog appears — the report is already on disk.

## 4. Public Interface

### 4.1 `vn::crash`（`sdk/vine/crash/CrashHandler.hpp`）

| Interface | Contract | Notes |
|---|---|---|
| `Config` | install-time configuration; an empty path = the default name beside the exe; `max_frames` falls in `[1, 64]` | read once, inside `install` |
| `Report` | a fixed-capacity crash payload (24 images / 64 frames / 8 context lines), trivially copyable | exactly the report file's content, shared both ways |
| `install(Config)` | hooks the last-chance filter and snapshots the configuration | true on Windows (SEH) and Linux (sigaction), false elsewhere |
| `uninstall()` | restores the filter and clears the snapshot; safe when not installed | |
| `renderReport(Report, span<char>)` | pure, allocates nothing; returns the bytes written, a prefix when the buffer is too small | what the crash path uses |
| `renderReport(Report)` | returns the complete text (for the handler and tests) | |
| `parseReport(text, Report&)` | skips unknown keys; a magic / version mismatch returns false | |

### 4.2 `CrashHandler.exe` (Linux: the extensionless `CrashHandler`)

```
CrashHandler --report <report file> [--dump <dump>] [--app <name>] [--mailto <address>]
```

An unknown switch is ignored (the crashing process may be a newer build). Without `--report` the exit code is 1; otherwise the window is the only output and the exit code is 0. Linux writes no dump, so `--dump` is not passed and the window shows “dump: (not produced)”.

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

- **C1** only the last-chance filter is hooked.
- **C2** one process, one set of facilities; `install` replaces idempotently.
- **C3** a report whose magic + version do not match is rejected whole; unknown keys are skipped.
- **C4** rendering emits whole lines only and stops at the first that does not fit (the prefix property).
- **C5** the symbol path = the handler's directory + the image's directory + `_NT_SYMBOL_PATH`.
- **C6** the Linux crash path calls async-signal-safe interfaces only (`read`/`write`/`open`/`fork`/`execv`/`raise`/`sigprocmask`); symbol resolution and text layout never happen in the crashing process.

### 5.2 Requirements (R#)

- **R1** `install` does not change the process's behaviour; after `uninstall` the filter is no longer called.
- **R2** `renderReport` / `parseReport` round-trip equal.
- **R3** with no symbols a frame still gives `module + offset`.

### 5.3 Invariants (N#)

- **N1** the crash path has no `new`/`malloc`, no standard streams, no logging module and no Qt.
- **N2** the report reaches disk before the dump: when the dump cannot be written the report is still there.
- **N3** report, dump and `.txt` share a directory and a stem (`.log` / `.dmp` / `.txt`).

## 6. Caveats and Traps

### 6.1 Windows macros and the entry point

- How it fails: `<excpt.h>` defines `exception_code` as `_exception_code`, so `Report::exception_code` becomes `Report::_exception_code` (`C2039`); `windows.h`'s `min`/`max` macros eat `std::min`; a `wmain` console handler flashes a console window on a crash.
- How to detect it: a member access reports “not a member of `Report`”, `std::min` reports a template error, a console window appears on a crash.
- How to prevent it: the field is called `code`; `#define NOMINMAX`; `WIN32_EXECUTABLE TRUE` + `wWinMain` + `CommandLineToArgvW`.

### 6.2 DbgHelp's base and size

- How it fails: with a base but no size the module counts as 0 bytes and every query fails (it looks like a missing PDB); when two images prefer the same base the later one displaces the earlier.
- How to detect it: a module whose PDB is local still has no function name and `loadedImage` is empty; two different indices return the same `LoadedImageName`.
- How to prevent it: read `SizeOfImage` from the PE header and pass it along; add an integer multiple of `0x10000000000` to a colliding base (see 2.5).

### 6.3 Re-entering the crash path

- How it fails: a second fault while writing the report (broken heap, an escalated stack overflow) re-enters the hook and two reports truncate each other.
- How to detect it: the report holds half a line, or its timestamps jump.
- How to prevent it: a monotonic latch (Windows `InterlockedExchange`, Linux `sig_atomic_t`), with a re-entry handed straight back to the system.

### 6.4 Linux: the stack and a distorted maps table

- How it fails: the `ucontext` PC goes on the stack first and `backtrace` yields the same address from the signal frame, so the top has two identical frames; a maps table larger than 64 KiB is truncated and a half line folds into a phantom image.
- How to detect it: the first two frames have identical addresses; the last image entry's path is suspiciously short, or a frame's offset is absurdly large.
- How to prevent it: when pushing, drop an adjacent duplicate of the previous frame; drop a last line with no newline whole, and merge images by path (see 2.9).

### 6.5 Two degradations on Linux

- How it fails: with no display in ssh / a service (or only plain Wayland with no XWayland) `XOpenDisplay` fails; with no `*_crash.dmp` beside it either, it easily looks like “the crash was not recorded”.
- How to detect it: the report and the `.txt` exist but there is no window; the report has no `dump` line.
- How to prevent it: the entry point checks `DISPLAY` first and prints the text to stdout on failure (the `.txt` is written first); see the backlog for dumps (kernel core / Crashpad).

## 7. Rejected Options

| # | Option | Why not |
|---|---|---|
| D1 | put it in `appfw` | a low-level need would depend on a high-level one (violates G2); a tool without Qt could not use it; touching Qt / logging on a crash is more dangerous. |
| D2 | fold it into `vn::system` | two things in one target, and every `system` consumer would link `dbghelp` (G5). |
| D3 | hook first-chance exceptions | it jumps ahead of the debugger and of `catch` and reports normal control flow as a crash (G4). |
| D4 | write through the logging module on a crash | the logging state is unknown (it may hold a lock) and it crashes a second time (G10). |
| D5 | install for every process by default | tests would swallow gtest's report and exit code (G8). |
| D6 | raise a Qt dialog from the crash handler | the heap may be broken and Qt may be the crash site itself (G10/G11). |
| D7 | pre-start a resident handler (the Crashpad shape) | today the need is only “start a process on a crash with a file as the interface”; residency is worth it only when “show a UI even with a broken heap” becomes a real pain (G8). |
| D8 | adopt Crashpad/Breakpad now | it would use only their cheapest local symbolization while taking on the whole build and deployment (G8). The trigger is in `../backlog/base-crash.md`. |
| D9 | have the handler walk the stack from the minidump | it would mean reimplementing StackWalk over the dump; the report's `module + offset` already suffices, and the dump is left to a real debugger (G7). |
| D10 | `Config::symbol_path` | the handler inherits the crashing process's environment and `_NT_SYMBOL_PATH` suffices; one more field is one more knob nobody sets (G8). |
| D11 | take the image table with `dl_iterate_phdr` on Linux | it takes the loader lock and the crash may happen inside `dlopen`; `/proc/self/maps` needs no lock (G10). |
| D12 | Qt Widgets for the Linux window | the window needs text, scrolling, buttons and a clipboard, and Xlib + Xft is enough; a toolkit would add a UI dependency to the `base` layer (G2). See `ADR-2026-10-30-crash-handler-x11-on-linux.md`. |
| D13 | symbolize inside the Linux crashing process | it needs to read DWARF and to allocate, which violates C6/N1; an external `addr2line` in the handler is safer (G7/G10). |

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
|---|---|---|
| T1 | `install` returns true on Windows and Linux; after `uninstall` the hook is no longer called | `test_crash`: `CrashInstallTest.*` |
| T2 | `uninstall()` is safe when not installed; a repeated `install` replaces | `test_crash`: `CrashInstallTest.UninstallIsSafe*`, `InstallingTwice*` |
| T3 | rendering in four shapes: no frames / with images / an address outside every image / with a context | `test_crash`: `CrashReportTest.*` |
| T4 | render and parse round-trip equal; non-report text is rejected; unknown keys are skipped | `test_crash`: `RoundTripsThroughParse`, `RejectsTextThatIsNotAReport`, `IgnoresKeys*` |
| T5 | with too small a buffer the output is a prefix of the complete report | `test_crash`: `DropsWholeLinesWhenTheBufferIsTooSmall` |
| T6 | a real crash: report + dump + a started handler + the exit code | manual: write through a null pointer in `main` temporarily, run once, check the three files and the symbol lines of the `.txt` |
| T7 | the image table: multi-segment merging, an anonymous range is not an image, a truncated half line is dropped, a frame keeps its bare address once the table is full | `test_crash`: `ImageTableTest.*` |
| T8 | a real SIGSEGV on Linux: report + `.txt` + window, exit code 139 | manual: `DISPLAY=:0 ./crashprobe <build>/bin/CrashHandler`; run on 2026-10-30, the `.txt` holds `main [crashprobe.cpp:22]` |

## 9. Status and Evolution

### 9.1 Current Status

- Windows: the library, the handler, the window and symbolization are all in place; `Vine.exe` installs before `createGuiApplication()` and the report's first line carries `Vine <version> (Debug|Release)`.
- Linux: `install` goes through `sigaction`; the report + image table (`/proc/self/maps`) + `addr2line` symbolization + the Xlib/Xft window all work.
- `test_crash` has 18 cases (including 6 in `ImageTableTest`); a real crash is exercised by T6 on Windows and T8 on Linux.

### 9.2 Known Gaps

- dump: Linux writes none, and a memory image there needs a kernel core (`RLIMIT_CORE`) or Crashpad (see the backlog); the Windows dump holds no memory beyond the thread context either, so a resident handler is assessed only once “a UI with a broken heap” matters.
- Linux symbolization depends on `addr2line` (binutils), and the image table comes from `/proc/self/maps` at crash time; when either is missing or does not fit (> 64 KiB) the `.txt` is still complete, only the frames degrade or fall back to a bare address. The window needs `libx11-dev` + `libxft-dev` (without them CMake does not build the handler, and the library still writes the report and the `.txt`).
- `std::set_terminate`, a stack-exhaustion guarantee (`SetThreadStackGuarantee`), a fallback from a read-only install directory to the user data directory, and a last-crash notice: see `../backlog/base-crash.md`.

### 9.3 Compatibility and Migration

- there is no hand-written `vineCrashFilter`/`installCrashLogger` in `main.cpp`; code that looked for `vn_crash.log` now finds `<app>_crash.log` (the report) + `.dmp` + the `.txt` the window writes.
- the report format only ever appends fields; the handler rejects an unknown version whole and displays the original text as it is.

## 10. References

### 10.1 Code

- `src/base/crash/sdk/vine/crash/CrashHandler.hpp` — the contract.
- `src/base/crash/src/` — the library (`vn::Crash`): `CrashCaptureWindows.cpp` (SEH filter / minidump / starting the handler), `CrashCaptureLinux.cpp` (sigaction / report / starting the handler), `ImageTable.cpp` (`/proc/self/maps` → the image table), `ReportRender.cpp`, `ReportParse.cpp`, `CrashHandler.cpp` (platform-neutral `install`/`uninstall` dispatch).
- `src/base/crash/handler/` — the window process: `mainWin.cpp`/`mainLinux.cpp` (entry points), `SymbolizeWin.cpp`/`SymbolizeLinux.cpp` (symbolization), `CrashWindowWin.cpp`/`CrashWindowX11.cpp` (the window), `ReportText.cpp` (the text layout shared by both platforms), `Text.cpp` (files and paths: wide / narrow overloads, UTF-16 on Windows and UTF-8 on POSIX).
- `src/apps/vine/src/main.cpp` — the only call site (installed first); `tests/test_crash/` — the cases.

### 10.2 Documents and ADRs

- `appfw-app-info.md` — where the identity on the report's first line comes from (`AppInfo`).
- `../decisions/ADR-2026-10-30-crash-handler-x11-on-linux.md` — why the Linux window is hand-drawn with Xlib + Xft, and what it costs.
- `../backlog/base-crash.md` — the trigger-gated work.

### 10.3 External Sources

- Win32 `SetUnhandledExceptionFilter`、`MiniDumpWriteDump`、`RtlCaptureStackBackTrace`；DbgHelp `SymLoadModuleEx`/`SymFromAddr`/`SymGetLineFromAddr64`；`TaskDialogIndirect`。
- POSIX `sigaction(SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT)`、`SA_RESETHAND`、`ucontext_t`、`/proc/self/maps`、`backtrace(3)`、`addr2line(1)`。
- the PE format: `IMAGE_OPTIONAL_HEADER.ImageBase` / `SizeOfImage`.
