# base/crash backlog

> Open items only; without a trigger condition it is not done. Closed items are deleted.

## 1. Actionable Now (No Trigger)

Empty. The current form (report + dump + window + symbolisation) already answers "where did it crash".

## 2. Trigger-gated

| # | Item | What | Trigger | Source |
|---|---|---|---|---|
| T1 | Telling the user about the last crash | on startup, check for an unread report and show a modeless prompt on the main window: "it crashed last time - open the report?" | the handler often fails to start (`CreateProcess` fails or it gets killed), or users keep saying "it crashed and the window just vanished" | `../design/base-crash.md` §9.2 |
| T2 | `std::set_terminate` | an uncaught C++ exception, or one thrown inside a `std::thread`, takes the same report path | a "the process just disappeared, no report" bug appears with `terminate` visible on the stack | ibid. |
| T3 | Guaranteeing stack space | `SetThreadStackGuarantee` plus writing the report on another thread when the stack overflows | a `0xC00000FD` crash appears whose report is missing or truncated | ibid. |
| T4 | Falling back from a read-only install directory | when installed under `%ProgramFiles%`, write reports to the user data directory instead | an installer puts the product in a read-only directory and the report file cannot be found on the user's machine | ibid. |
| T5 | Linux dumps | have the kernel write a core (or fork a memory image) on a crash: Linux only produces a report today, with nothing for a debugger | a Linux crash appears where "the frames the report points at are not enough, the memory is needed" | `../design/base-crash.md` §9.2 |
| T6 | Switching to Crashpad / Breakpad | replace the in-house chain and adopt a full handler + dump tool chain | 1) a cross-platform minidump in one format is needed; 2) reporting/aggregation from user machines is needed (Sentry and the like included); 3) a symbol server or batch symbolisation is needed | `../design/base-crash.md` §7 D8 |
| T7 | Mail / packaged upload | pack the `.txt` + dump and send it to one definite support channel | the support channel and its recipients are settled, and "the user drags the attachment by hand" measures as a high failure rate | `../design/base-crash.md` §2.6 |
| T8 | Parsing dumps | the handler walks the stack itself from a minidump instead of trusting the frames the crashing process recorded | the frames in a report being insufficient (a truncated stack buffer, say) becomes the bottleneck | `../design/base-crash.md` §7 D9 |
| T9 | Symbolising without an external `addr2line` | read DWARF directly on Linux (or link a DWARF library) and drop the run-time dependency on binutils | the user's machine has no `addr2line`, and the `.txt` says "addr2line could not be started" | `../design/base-crash.md` §2.4, §9.2 |
| T10 | A module table larger than 64 KiB | when `/proc/self/maps` does not fit, only the first 24 images are kept; change it to fold while reading instead of caching the whole text | in a deployment with many plugins, crash frames often fall in a dropped image | `../design/base-crash.md` §2.9, §9.2 |
