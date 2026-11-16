#!/usr/bin/env python3
"""Check that the shared graphics mechanisms' API is exported (they are part of libGraphics, not a private copy).

The mechanisms both backends use - the frame gate, the device clock and its frame timeline, retirement,
readback, the draw-state keys, the target plan and the refusal draft - are compiled into libGraphics,
so a backend plugin resolves their symbols across the graphics DLL boundary: every type and free
function declared there with an OUT-OF-LINE definition needs VN_GRAPHICS_API. On Windows a missing
macro is an unresolved external at the plugin's link; on Linux the macro expands to nothing, so the
mistake would go unnoticed here - this script is what makes the rule checkable on every platform.
Data-only structs and enums need no macro (they have no symbols), and forward declarations are not
declarations of an API.

The scope is an explicit FILE LIST on purpose. Once these headers lived in their own directory and the
scope could be a path; they now sit in the SDK root beside the host half, whose nested implementation
types are defined out of line but never called across the boundary - so "defined out of line" is not
by itself a reason to export there, and the list is what keeps the check meaningful.

Usage: check_export_annotations.py [sdk-dir-or-file src-dir-or-file]...
"""

import pathlib
import re
import sys

# One entry per set of files compiled into a shared library and consumed from outside it. An entry
# may be a directory (scanned recursively) or a single file; this one is a FILE LIST because the
# shared mechanisms now live beside the host half (see the module note above).
SHARED_MECHANISM_HEADERS = (
    "DeviceClock.hpp",
    "DiagnosticDraft.hpp",
    "DrawState.hpp",
    "FrameGate.hpp",
    "FrameTimeline.hpp",
    "Readback.hpp",
    "RetirementQueue.hpp",
    "TargetPlan.hpp",
)
SHARED_MECHANISM_SOURCES = (
    "DeviceClock.cpp",
    "DiagnosticDraft.cpp",
    "DrawState.cpp",
    "FrameTimeline.cpp",
    "Readback.cpp",
    "RetirementQueue.cpp",
    "TargetPlan.cpp",
)
HALVES = ((
    tuple(f"src/viz/graphics/sdk/vine/graphics/{name}" for name in SHARED_MECHANISM_HEADERS),
    tuple(f"src/viz/graphics/src/{name}" for name in SHARED_MECHANISM_SOURCES),
),)

# Where private headers live (the appfw convention: next to their .cpp, included with "..."). A
# declaration that crosses the DLL boundary must sit in the installed API root instead; this mirror
# rule is what keeps the two from drifting back together.
PRIVATE_HEADER_ROOTS = ("src/viz/graphics/src",)

# A member definition. The member name may be an OPERATOR (`VertexLayoutKey::operator==(`), which a
# `\w+\s*\(` pattern cannot see: that blind spot is how a key's operator== shipped without its export
# macro and broke the plugin's link, so the operator forms are spelled out here.
OWNER = re.compile(r"\b([A-Za-z_]\w*)::(?:~?[A-Za-z_]\w*|operator\s*[^\s(]*)\s*\(")
# The macro sits between the keyword and the name once a declaration IS annotated, so it must be
# allowed there - otherwise an annotated type is captured as "VN_GRAPHICS_API" and stops being checked.
DECL = re.compile(r"^\s*(?:class|struct)\s+(?:VN_GRAPHICS_API\s+)?([A-Za-z_]\w*)")
FUNC_DECL = re.compile(r"^(?!static|inline|template|using|typedef|friend|extern|#|//|/\*|\})"
                       r"(?:\[\[nodiscard\]\]\s+)?[A-Za-z_][\w:<>, \s*&]*?\b([A-Za-z_]\w*)\s*\(")


def names_a_member(line, name):
    """Return whether @p name is qualified on @p line, i.e. the line defines a member rather than a
    free function.

    A free function's RETURN TYPE or PARAMETERS may name a qualified type (`std::size_t size(`,
    `ReadbackFormat colorReadbackOf(vn::graphics::RenderTarget::ColorFormat)`), and skipping every
    line containing `::` - the check's second blind spot - dropped exactly those declarations.
    """
    return re.search(r"::\s*" + re.escape(name) + r"\s*\(", line) is not None


def defined_out_of_line(sources):
    """Return the type and free function names the sources define out of line."""
    owners, functions = set(), set()
    for path in sources:
        for line in path.read_text(encoding="utf-8", errors="ignore").split("\n"):
            if not line or line[0].isspace() or line.startswith(("//", "#", "}")):
                continue
            match = OWNER.search(line)
            if match and match.group(1) not in ("std", "vn"):
                owners.add(match.group(1))
                continue
            match = re.match(r"^[A-Za-z_][\w:<>,\s*&]*?\b([A-Za-z_]\w*)\s*\(", line)
            if match and not names_a_member(line, match.group(1)):
                functions.add(match.group(1))
    return owners, functions


def declarations(headers):
    """Return {name: (path, number, window)} for every declaration at namespace scope."""
    found = {}
    for path in headers:
        lines = path.read_text(encoding="utf-8", errors="ignore").split("\n")
        for number, line in enumerate(lines, start=1):
            stripped = line.lstrip()
            if stripped.startswith(("//", "*", "/*")):
                continue
            match = DECL.match(line)
            if match and not line.rstrip().endswith(";"):
                found.setdefault(match.group(1), (path, number, "\n".join(lines[number - 1:number + 2])))
                continue
            match = FUNC_DECL.match(line)
            if match:
                found.setdefault(match.group(1), (path, number, "\n".join(lines[number - 1:number + 2])))
    return found


def expand(entries, pattern):
    """Return the files named by @p entries: directories are scanned, single files are taken as they are."""
    files = []
    for raw in entries:
        path = pathlib.Path(raw)
        if path.is_dir():
            files.extend(sorted(path.rglob(pattern)))
        else:
            files.append(path)
    return files


def label_of(paths):
    """Return a short name for one side of the check, for the summary line."""
    first = pathlib.Path(paths[0])
    return f"{first.parent.name}/{first.name}" if first.is_file() else first.name


def check(sdk_paths, src_paths):
    """Return the findings for one set of files as (path, line, message) triples and the annotation ratio."""
    if isinstance(sdk_paths, (str, pathlib.Path)):
        sdk_paths = (sdk_paths,)
    if isinstance(src_paths, (str, pathlib.Path)):
        src_paths = (src_paths,)

    missing = [raw for raw in (*sdk_paths, *src_paths) if not pathlib.Path(raw).exists()]
    if missing:
        # A check that reports "0/0" for a file that has moved is a false green (that happened once):
        # a missing entry is a finding, never an empty result.
        return [(pathlib.Path(missing[0]), 0, "the file does not exist - is the path right?")], 0, 0
    owners, defined = defined_out_of_line(expand(src_paths, "*.cpp"))
    declared = declarations(expand(sdk_paths, "*.hpp"))

    findings, annotated = [], 0
    for name in sorted(set(declared) & owners) + sorted(n for n in declared if n in defined):
        path, number, window = declared[name]
        if f"VN_GRAPHICS_API {name}" in window or (name in window and "VN_GRAPHICS_API" in window):
            annotated += 1
            continue
        line = window.split("\n")[0].strip()
        findings.append((path, number,
                         f"'{name}' has out-of-line definitions but its declaration carries no "
                         f"VN_GRAPHICS_API: {line[:90]}"))
    return findings, annotated, len(set(declared) & owners) + len([n for n in declared if n in defined])


def private_header_findings():
    """Return the findings for exported declarations that live in a private header.

    The mirror of the rule above: `install(DIRECTORY sdk/)` is what makes the export surface match
    the installed declarations, so anything exported belongs in `sdk/`. A header under `src/` is
    private to this module - it is not on any include root, so nothing outside can include it - and
    it must therefore not carry an export macro at all.
    """
    findings = []
    for root in PRIVATE_HEADER_ROOTS:
        for path in sorted(pathlib.Path(root).rglob("*.hpp")):
            for number, line in enumerate(path.read_text(encoding="utf-8", errors="ignore").split("\n"), 1):
                if "VN_GRAPHICS_API" in line:
                    findings.append((path, number,
                                     "a private header declares an exported symbol (move the declaration "
                                     f"to sdk/, or drop the macro): {line.strip()[:80]}"))
    return findings


def main():
    halves = []
    arguments = sys.argv[1:]
    for index in range(0, len(arguments) - 1, 2):
        halves.append((arguments[index], arguments[index + 1]))
    halves = halves or list(HALVES)

    total = 0
    for sdk_paths, src_paths in halves:
        findings, annotated, needed = check(sdk_paths, src_paths)
        for path, number, message in findings:
            print(f"{path}:{number}: {message}")
        total += len(findings)
        if needed == 0 and not findings:
            print(f"{label_of(sdk_paths)}:0: no exported entity found here - a moved file would report a false green")
            total += 1
        print(f"--- {label_of(sdk_paths)}: {annotated}/{needed} exported entity(ies) annotated")

    private = private_header_findings()
    for path, number, message in private:
        print(f"{path}:{number}: {message}")
    total += len(private)
    print(f"--- {total} export-annotation finding(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
