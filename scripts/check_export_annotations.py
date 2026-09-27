#!/usr/bin/env python3
"""Check that the backend half's API is exported (it is part of libGraphics, not a private copy).

`vine/graphics/backend/` is compiled into libGraphics, so a backend plugin resolves its symbols across
the graphics DLL boundary: every type and free function declared there with an OUT-OF-LINE definition
needs VN_GRAPHICS_API. On Windows a missing macro is an unresolved external at the plugin's link; on
Linux the macro expands to nothing, so the mistake would go unnoticed here - this script is what makes
the rule checkable on every platform. Data-only structs and enums need no macro (they have no
symbols), and forward declarations are not declarations of an API.

The scope is the backend half on purpose: it is the part whose whole audience is outside the library.
The host half also contains nested implementation types whose methods are defined out of line but are
never called across the boundary, so "defined out of line" is not by itself a reason to export there.

Usage: check_export_annotations.py [sdk-dir src-dir]...
"""

import pathlib
import re
import sys

# One entry per half that is compiled into a shared library and consumed from outside it.
HALVES = (("src/viz/graphics/sdk/vine/graphics/backend", "src/viz/graphics/src/backend"),)

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


def check(sdk_dir, src_dir):
    """Return the findings for one half as (path, line, message) triples and the annotation ratio."""
    sdk = pathlib.Path(sdk_dir)
    src = pathlib.Path(src_dir)
    if not sdk.is_dir() or not src.is_dir():
        # A check that reports "0/0" for a path that has moved is a false green (that happened once):
        # a missing half is a finding, never an empty result.
        missing = sdk_dir if not sdk.is_dir() else src_dir
        return [(pathlib.Path(missing), 0, "the half does not exist - is the path right?")], 0, 0
    owners, defined = defined_out_of_line(sorted(src.rglob("*.cpp")))
    declared = declarations(sorted(sdk.rglob("*.hpp")))

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
    for sdk_dir, src_dir in halves:
        findings, annotated, needed = check(sdk_dir, src_dir)
        for path, number, message in findings:
            print(f"{path}:{number}: {message}")
        total += len(findings)
        if needed == 0 and not findings:
            print(f"{sdk_dir}:0: no exported entity found here - a moved half would report a false green")
            total += 1
        print(f"--- {pathlib.Path(sdk_dir).name}: {annotated}/{needed} exported entity(ies) annotated")

    private = private_header_findings()
    for path, number, message in private:
        print(f"{path}:{number}: {message}")
    total += len(private)
    print(f"--- {total} export-annotation finding(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
