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
    VsgProgramSlot.hpp included vine/graphics/Camera.hpp twice.  `#pragma once` makes
    them harmless, which is exactly why nobody notices them.

It also keeps the graphics module free of a render API and of any backend (one rule, same theme: a
dependency the compiler cannot see). The property is about DEPENDENCY, not about a directory: one
`vsg/` include under `graphics/` would tie every backend, present and future, to vsg (see
graphics_module_findings).

A third rule pins the SEAM - what the two backends share from the SDK - by the property that makes it
a seam (both backends need it), not by a path set: the eight mechanism headers must each be reachable
from both backends, and the set of SDK headers both backends include directly is recorded, so a header
entering or leaving that set is a decision this check asks for. See SEAM_HEADERS.

The same fact is stated twice more for two audiences - the argument in `.ai/design/graphics-architecture.md`
2.9 and the reader's table in `docs/guides/graphics-backends.md` 5 - so the check also requires each of
those pages to NAME every mechanism the list holds (SEAM_DOCS). The list is the decision; a page that
stops naming one of its mechanisms is drift, and a mechanism added to the list without a page naming it
is an undocumented decision. Coverage is checked, staleness is not: both pages name mechanisms in other
sections too, so "a page says a mechanism the list does not hold" cannot be decided from prose.

In a .cpp the FIRST include is the file's own header (the rule says so), so it is
not part of the group order — but the group ORDER is only checked in headers:

  * a header has one owner and one obvious order, so the rule applies verbatim;
  * a plugin's .cpp files open with the module's own headers (its own header plus
    the private ones it shares with sibling TUs, e.g. ContentDraw.cpp's run of
    vine/backend_vk/internal/*.hpp) before the std / third-party groups.  Reordering those is a
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
SKIP_PARTS = ("build/", "_deps/")
# The graphics module (its SDK and its sources) must stay API-agnostic (see
# .ai/design/graphics-architecture.md): it may include the standard library and its own headers, but
# nothing from a render API and nothing from a backend plugin. The old rule was stated over a
# `backend/` directory; the property it protected is about DEPENDENCY, not about a path - one
# `vine/backend_vk/...` include here ties the module to that backend, and the list grows with each
# backend that lands.
GRAPHICS_MODULE_DIRS = ("/graphics/src/", "/vine/graphics/")
GRAPHICS_FORBIDDEN_PREFIXES = ("vine/backend_vk/",)


def is_graphics_module(path):
    """Return whether @p path belongs to the graphics module (its SDK headers or its sources)."""
    text = str(path).replace("\\", "/")
    return any(marker in text for marker in GRAPHICS_MODULE_DIRS)


def graphics_module_findings(path, lines):
    """Return the findings that keep the graphics module free of the render API and of any backend."""
    if not is_graphics_module(path):
        return []

    findings = []
    for number, line in enumerate(lines, start=1):
        match = INCLUDE.match(line)
        if not match:
            continue
        header = match.group(2)
        for prefix in GRAPHICS_FORBIDDEN_PREFIXES:
            if header.startswith(prefix):
                findings.append(
                    (number, f"the graphics module includes a render API or a backend ({header}): {line.strip()}"))
                break
    return findings


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


# A rule that finds no file is a false green (that has happened twice in this tree): the graphics
# module's two roots are named here, and a missing one is a finding rather than an empty result.
GRAPHICS_MODULE_ANCHORS = ("src/viz/graphics/sdk/vine/graphics", "src/viz/graphics/src")


# ---- the seam: what BOTH backends share from the SDK -------------------------------
# The seam was once a `backend/` directory with a SEAM_HEADERS list; the ADR deleted the directory and
# the list never existed, so for a while nothing asserted the set at all (audit A3-04). A list of paths
# cannot protect anything - the property is "both backends need it", not "it lives here" - so this
# check records the property's RESULT and fails when it changes:
#
#   * the eight mechanism headers both backends need, each reachable from both (directly, or through
#     another seam header: `FrameTimeline.hpp` reaches vk through `DeviceClock.hpp`);
#   * the SDK headers both backends include DIRECTLY. That set is the shared surface: when it changes,
#     the change is a decision (a new mechanism joins the seam, or a backend starts using the model
#     API), so the check asks for SEAM_SHARED_HEADERS to be updated in the same commit.
#
# What this check does NOT decide: whether one of the shared headers is a mechanism or part of the
# public model surface. That is a design judgment; the list is where it gets recorded, and the
# docstring of `graphics-backends.md` §5 (and `graphics-architecture.md` §2.9) is where it is argued.
SEAM_HEADERS = frozenset((
    "DeviceClock.hpp",
    "DiagnosticDraft.hpp",
    "DrawState.hpp",
    "FrameGate.hpp",
    "FrameTimeline.hpp",
    "Readback.hpp",
    "RetirementQueue.hpp",
    "TargetPlan.hpp",
))
# The two pages that describe the seam. Each must name every mechanism SEAM_HEADERS holds: the list is the
# decision (see ADR-2026-11-18-seam-identity-is-a-property-not-a-namespace.md), and a page that silently stops
# naming one of them would be the drift this rule exists to catch. Names are looked for in their backticked
# spelling, which is how both pages list them.
SEAM_DOCS = (
    ".ai/design/graphics-architecture.md",  # 2.9, the argument
    "docs/guides/graphics-backends.md",     # 5, the reader's table
)
# Measured 2026-11-23, after the reference backend retired (ADR-2026-11-23 D4 step 4): the SDK headers the
# SHIPPED backend includes directly, extended by each batch that widens what it reaches. Before the retirement this set
# was the intersection of two backends (25); with one reader it is "what the backend reaches", and the promise rule
# still makes a change here a decision about the seam or about the model surface, not about the backend that happens to
# need it.
SEAM_SHARED_HEADERS = frozenset((
    "BackendAbi.hpp",
    "BuiltinShaders.hpp",
    "DepthRange.hpp",
    "DeviceClock.hpp",
    "DiagnosticDraft.hpp",
    "Diagnostics.hpp",
    "DrawAbi.hpp",
    "DrawState.hpp",
    "FrameGate.hpp",
    "FrameReport.hpp",
    "Geometry.hpp",
    "LightAbi.hpp",
    "MaterialAbi.hpp",
    "Readback.hpp",
    "RenderBackend.hpp",
    "RenderBackendRegistry.hpp",
    "RenderState.hpp",
    "RenderTarget.hpp",
    "ReportOnce.hpp",
    "RetirementQueue.hpp",
    "Sampler.hpp",
    "ShaderAbi.hpp",
    "ShaderProgram.hpp",
    "StateResolution.hpp",
    "TargetPlan.hpp",
    "Texture.hpp",
    "Topology.hpp",
    "WindowPresentTarget.hpp",
    "Capabilities.hpp",
    "FrameDescription.hpp",
    "MaterialValues.hpp",
    "PolygonMode.hpp",
    "PrepareReport.hpp",
    "PresentTarget.hpp",
    "Primitive.hpp",
    "ProgramVariant.hpp",
    "RenderDiagnostic.hpp",
    "PassSelection.hpp",
))
SDK_GRAPHICS_ROOT = "src/viz/graphics/sdk/vine/graphics"
BACKEND_ROOTS = ("src/plugins/gfx_backend_vk",)
GRAPHICS_INCLUDE = re.compile(r'^#include\s*[<"]vine/graphics/([^>"]+)[>"]')


def graphics_includes_of(root):
    """Return the SDK graphics headers one backend's sources include directly."""
    names = set()
    for path in pathlib.Path(root).rglob("*"):
        if path.suffix not in (".hpp", ".cpp"):
            continue
        for line in path.read_text(encoding="utf-8", errors="ignore").split("\n"):
            match = GRAPHICS_INCLUDE.match(line)
            if match:
                names.add(match.group(1))
    return names


def seam_findings():
    """Return the findings that keep the seam's declared set honest (see SEAM_HEADERS)."""
    root = pathlib.Path(__file__).resolve().parent.parent
    sdk = root / SDK_GRAPHICS_ROOT
    if not sdk.is_dir():
        return [f"{SDK_GRAPHICS_ROOT}: the SDK root is gone - the seam rule would silently cover nothing"]
    missing_roots = [backend for backend in BACKEND_ROOTS if not (root / backend).is_dir()]
    if missing_roots:
        return [f"{backend}: the backend root is gone - the seam rule would silently cover nothing"
                for backend in missing_roots]

    direct = {backend: graphics_includes_of(root / backend) for backend in BACKEND_ROOTS}
    shipped = direct[BACKEND_ROOTS[0]]

    def with_one_hop(names):
        """The direct includes plus what those SDK headers include (the seam is one hop deep)."""
        reached = set(names)
        for name in names:
            header = sdk / name
            if not header.is_file():
                continue
            for line in header.read_text(encoding="utf-8", errors="ignore").split("\n"):
                match = GRAPHICS_INCLUDE.match(line)
                if match:
                    reached.add(match.group(1))
        return reached

    reached = {backend: with_one_hop(names) for backend, names in direct.items()}
    findings = []
    for name in sorted(SEAM_HEADERS):
        if not (sdk / name).is_file():
            findings.append(f"{SDK_GRAPHICS_ROOT}/{name}: the seam list names a header that is gone")
            continue
        for backend in BACKEND_ROOTS:
            if name not in reached[backend]:
                findings.append(
                    f"{name}: the seam list says the shipped backend needs it, but {backend} neither includes it"
                    " nor includes a seam header that does")

    for name in sorted(shipped - SEAM_SHARED_HEADERS):
        findings.append(
            f"{name}: the shipped backend now includes it directly, so it is part of the surface the seam"
            " reaches - decide whether it is a mechanism (add to SEAM_HEADERS) or model surface, then add it to"
            " SEAM_SHARED_HEADERS")
    for name in sorted(SEAM_SHARED_HEADERS - shipped):
        findings.append(
            f"{name}: SEAM_SHARED_HEADERS says the shipped backend includes it directly, but that is no longer"
            " true - update the list (and SEAM_HEADERS if it was a mechanism)")
    findings.extend(seam_doc_findings(root))
    return findings


def seam_doc_findings(root):
    """Return the findings that keep the pages describing the seam in step with SEAM_HEADERS."""
    findings = []
    for relative in SEAM_DOCS:
        page = root / relative
        if not page.is_file():
            findings.append(f"{relative}: the page that states the seam is gone - the doc rule would silently"
                            " cover nothing")
            continue
        text = page.read_text(encoding="utf-8", errors="replace")
        for name in sorted(SEAM_HEADERS):
            mechanism = name[: -len(".hpp")]
            if f"`{mechanism}`" not in text:
                findings.append(
                    f"{relative}: the seam list holds `{mechanism}`, and this page no longer names it -"
                    " the list is the decision (SEAM_HEADERS), so state it or change the list")
    return findings


def graphics_module_file_set_findings():
    """Return a finding when either graphics root is gone, so the rule above cannot silently scan nothing."""
    root = pathlib.Path(__file__).resolve().parent.parent
    findings = []
    for folder in GRAPHICS_MODULE_ANCHORS:
        directory = root / folder
        if not directory.is_dir() or not (any(directory.rglob("*.hpp")) or any(directory.rglob("*.cpp"))):
            findings.append(
                f"{folder}: the graphics root is gone - the API-agnostic rule would silently cover nothing")
    return findings


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
    findings.extend(graphics_module_findings(path, lines))
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
    if not files:
        # A run that scans nothing reports green for free - the same false green a renamed path produces.
        print("no file to scan: pass a path, or run with no arguments to check the default tree")
        total += 1
    for path in files:
        for number, message in check_file(path):
            print(f"{path}:{number}: {message}")
            total += 1
    for message in graphics_module_file_set_findings():
        print(message)
        total += 1
    for message in seam_findings():
        print(message)
        total += 1
    print(f"--- {total} include-hygiene finding(s) in {len(files)} file(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
