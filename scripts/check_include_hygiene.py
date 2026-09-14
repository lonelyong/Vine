#!/usr/bin/env python3
"""check_include_hygiene.py — report include-GROUP inversions and duplicated includes.

The repository's include rule (see .github/copilot-instructions.md) is a group
order, not an alphabet:

    1. the project's xxxGlobal.hpp (first)
    2. C++ standard library
    3. third-party libraries
    4. local libraries (vine/...)
    5. same-directory headers (quoted)

Two kinds of defect are invisible to the compiler and therefore rot silently:

  * a group out of order (a `<vector>` after the local group), which makes the
    file's dependencies unreadable — and hides the next one;
  * an include written TWICE.  A real example lived in VsgContentSlot.cpp: the
    file's own header at line 1 and again among the plugin headers, left behind
    by the script that merged the public renderer header (§44) into each TU;
    VsgOverlay.hpp included vine/graphics/Camera.hpp twice.  `#pragma once` makes
    them harmless, which is exactly why nobody notices them.

In a .cpp the FIRST include is the file's own header (the rule says so), so it is
not part of the group order — but the group ORDER is only checked in headers:

  * a header has one owner and one obvious order, so the rule applies verbatim;
  * a plugin's .cpp files open with the module's own headers (its own header plus
    the private ones it shares with sibling TUs, e.g. VsgRenderer.cpp's run of
    vine/vsg/*.hpp) before the std / third-party groups.  Reordering those is a
    separate decision about that house style, not a typo to gate on, and a gate
    that fires 123 times on it would be turned off instead of followed.

Duplicate includes are checked in both, and a .cpp's own header written a SECOND
time among the plugin headers is reported (that is the §44 leftover this check was
written after).

Usage: python3 scripts/check_include_hygiene.py [paths...]
       (defaults to src, tests and tools — the whole tree, which is clean)
Exit status: 1 when something is suspicious, so it can gate a build.

History worth knowing before trusting a number from this check: the first run of
this script over `src/` reported 133 findings, of which 100 were ITS OWN bug — the
standard-header list was missing <coroutine>, <format>, <source_location>,
<stop_token>, <cerrno>, <concepts>, <semaphore>, <iosfwd> and <ostream>, so those
read as third-party and every std group after them looked inverted. The real
count was 33, all one pattern (a module's own headers before <std>), and all 33
are fixed. A finding that says "std group after third-party" is therefore worth a
second look at STD_HEADERS before it is worth an edit.
"""
import pathlib
import re
import sys

GLOBAL_HEADER = 1
STD = 2
THIRD_PARTY = 3
LOCAL = 4
SAME_DIR = 5
GROUP_NAMES = {STD: "std", THIRD_PARTY: "third-party", LOCAL: "local", SAME_DIR: "same-dir"}

# C++ standard headers. A header that is not listed here is treated as a library
# header, so a new standard header must be added HERE or it reads as third-party
# and every std group after it looks inverted (measured once: <coroutine>,
# <format>, <source_location> and friends hid the real state of base/async).
STD_HEADERS = frozenset((
    "algorithm", "any", "array", "atomic", "bit", "bitset", "cassert", "cctype", "cerrno", "cfenv",
    "cfloat", "charconv", "chrono", "cinttypes", "climits", "clocale", "cmath", "codecvt", "compare",
    "complex", "concepts", "condition_variable", "coroutine", "csetjmp", "csignal", "cstdarg",
    "cstddef", "cstdint", "cstdio", "cstdlib", "cstring", "ctime", "cuchar", "cwchar", "cwctype",
    "deque", "exception", "execution", "expected", "filesystem", "flat_map", "flat_set", "format",
    "forward_list", "fstream", "functional", "future", "generator", "initializer_list", "iomanip",
    "ios", "iosfwd", "iostream", "istream", "iterator", "latch", "limits", "list", "locale", "map",
    "mdspan", "memory", "memory_resource", "mutex", "new", "numbers", "numeric", "optional",
    "ostream", "print", "queue", "random", "ranges", "ratio", "regex", "semaphore", "set",
    "shared_mutex", "source_location", "span", "sstream", "stack", "stdexcept", "stop_token",
    "streambuf", "string", "string_view", "strstream", "syncstream", "system_error", "thread",
    "tuple", "type_traits", "typeindex", "typeinfo", "unordered_map", "unordered_set", "utility",
    "valarray", "variant", "vector", "version",
    # the C headers spelled with .h
    "assert.h", "ctype.h", "errno.h", "float.h", "inttypes.h", "limits.h", "locale.h", "math.h",
    "setjmp.h", "signal.h", "stdarg.h", "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "string.h",
    "time.h", "uchar.h", "wchar.h", "wctype.h",
))

INCLUDE = re.compile(r'^#include\s*([<"])([^>"]+)[>"]\s*$')
DEFAULT_PATHS = ("src", "tests", "tools")
SKIP_PARTS = ("build/", "_deps/", "/vsg_selftest/")


def group_of(quote, header):
    """Return the include group of one header, or None when it is not grouped."""
    if header.endswith("_global.hpp") or header.endswith("Global.hpp"):
        return GLOBAL_HEADER
    if header.split("/")[0] == "vine":
        return LOCAL
    if header in STD_HEADERS:
        return STD
    if quote == '"' and "/" not in header:
        return SAME_DIR
    return THIRD_PARTY


def check_file(path):
    """Return the findings for one file as (line, message) pairs."""
    lines = path.read_text(encoding="utf-8", errors="ignore").split("\n")
    includes = []
    for number, line in enumerate(lines, start=1):
        match = INCLUDE.match(line)
        if match:
            includes.append((number, group_of(match.group(1), match.group(2)), match.group(0)))

    findings = []
    if path.suffix == ".hpp":
        highest = 0
        for number, group, text in includes:
            if group == GLOBAL_HEADER:
                continue
            if group < highest:
                findings.append(
                    (number, f"{GROUP_NAMES[group]} group after {GROUP_NAMES[highest]}: {text}"))
            else:
                highest = group

    seen = {}
    for number, _, text in includes:
        if text in seen:
            findings.append((number, f"duplicated (also on line {seen[text]}): {text}"))
        else:
            seen[text] = number
    return findings


def collect(arguments):
    """Expand the command line into the files to check."""
    paths = arguments or list(DEFAULT_PATHS)
    files = []
    for raw in paths:
        path = pathlib.Path(raw)
        if path.is_file():
            files.append(path)
        else:
            files.extend(sorted(path.rglob("*.hpp")))
            files.extend(sorted(path.rglob("*.cpp")))
    return [f for f in files if not any(part in str(f) for part in SKIP_PARTS)]


def main():
    files = collect(sys.argv[1:])
    total = 0
    for path in files:
        for number, message in check_file(path):
            print(f"{path}:{number}: {message}")
            total += 1
    print(f"--- {total} include-hygiene finding(s) in {len(files)} file(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
