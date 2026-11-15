#!/usr/bin/env python3
"""check_diagnostic_formats.py — report diagnostic / log calls whose argument count
looks wrong.

The backend builds its diagnostics from a printf-style literal plus the failing
data's numbers, so a literal with more (or fewer) conversions than arguments, or a
format chosen by a ternary whose arguments only match ONE branch, prints wrong
numbers silently — it compiles, it runs, and the message lies. One such call was
found in SceneBridgeGeometry (the loc1-normal rejection printed the component count
where the float count belongs), which is what this check exists to prevent.

Two families are checked:
  * formatDiagnostic(u8"<fmt>", args...)   -> %-conversions vs top-level commas
  * VN_LOGI / VN_LOGW / VN_LOGE / VN_LOGD(...) -> {} placeholders vs top-level commas

Usage: python3 scripts/check_diagnostic_formats.py [files...]
       (defaults to the VSG backend plugin's sources and its selftest)
Exit status: 1 when something is suspicious, so it can gate a build.
"""
import pathlib
import re
import sys

SPEC = re.compile(r"%[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|z|j|t|L)?[diuoxXfFeEgGaAcspn]")
PLACEHOLDER = re.compile(r"\{[^}]*\}")
MACROS = ("VN_LOGI", "VN_LOGW", "VN_LOGE", "VN_LOGD")

# --- diagnostic CODES: a stable id per cause, a registry, and a ratchet (audit A3) ---
#
# A diagnostic a host cannot match on without parsing prose is a log line. The SDK therefore carries a stable `code`
# (see RenderDiagnostic), each module names its own (`vk.<class>.<cause>`), and this file keeps the two halves in
# step: every code a source file states is registered with a meaning, every registered code is used, and a report
# call that states NO code may not grow past the ceiling `diagnostic_code_baseline.txt` records for its file.
CODE_RE = re.compile(r'u8"([a-z][a-z0-9_]*(?:\.[a-z0-9_]+){2,})"')
ASSIGNED_CODE_RE = re.compile(r'\.code\s*=\s*(?:vn::String\()?u8"([a-z][a-z0-9_]*(?:\.[a-z0-9_]+){2,})"')
REPORT_NAMES = ("reportDiagnostic", "reportEngineProblem", "noteProblem")
CODES_FILE = "scripts/diagnostic_codes.txt"
BASELINE_FILE = "scripts/diagnostic_code_baseline.txt"
DEFAULT_GLOBS = (
    # `**` because the plugin's sources live in layers (`src/{shell,internal,support}`): the flat
    # `src/*.cpp` this used to be went from 7 files to 0 the moment they moved, and the script still
    # reported "0 suspicious call(s)" - which is why a pattern matching nothing is a finding below.
    "src/plugins/gfx_backend_vsg/src/**/*.cpp",
)


def split_calls(text, name):
    """Yield (line, call_body) for every `name(` occurrence."""
    for match in re.finditer(r"\b" + re.escape(name) + r"\(", text):
        index, depth, in_string, escaped = match.end(), 1, None, False
        while index < len(text) and depth:
            char = text[index]
            if in_string:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == in_string:
                    in_string = None
            elif char in ('"', "'"):
                in_string = char
            elif char in "([{":
                depth += 1
            elif char in ")]}":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        yield text[: match.start()].count("\n") + 1, text[match.end():index]


def split_top(body, separators):
    """Split on top-level @p separators, ignoring nesting and string literals."""
    parts, buffer, depth, in_string, escaped = [], "", 0, None, False
    for char in body:
        if in_string:
            buffer += char
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == in_string:
                in_string = None
            continue
        if char in ('"', "'"):
            in_string = char
            buffer += char
            continue
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        if char in separators and depth == 0:
            parts.append(buffer)
            buffer = ""
        else:
            buffer += char
    parts.append(buffer)
    return parts


def branches(format_arg):
    """The format-literal groups of a (possibly ternary) format argument."""
    if "?" not in format_arg:
        return [format_arg]
    return [arm for arm in split_top(format_arg, "?:") if '"' in arm]


def conversions(literal, kind):
    """Number of conversions the literal consumes."""
    if kind == "%":
        return len(SPEC.findall(literal.replace("%%", "")))
    return len(PLACEHOLDER.findall(literal.replace("{{", "").replace("}}", "")))


def literal_of(argument):
    """The string literals of a format argument, concatenated."""
    return "".join(re.findall(r'"(?:[^"\\]|\\.)*"', argument))


def check(path):
    """Report every suspicious call in one file; returns how many were found."""
    text = pathlib.Path(path).read_text(encoding="utf-8")
    found = 0
    checks = [("formatDiagnostic", "%")] + [(macro, "{") for macro in MACROS]
    for name, kind in checks:
        for line, body in split_calls(text, name):
            args = split_top(body, ",")
            if len(args) < 2:
                continue
            provided = len(args) - 1
            for arm in branches(args[0]):
                literal = literal_of(arm)
                wanted = conversions(literal, kind) if literal else 0
                if wanted == 0 or wanted == provided:
                    continue
                found += 1
                print(f"{path}:{line}: {name}: {wanted} conversion(s) vs {provided} argument(s)")
                print(f"    fmt: {literal[:130]}")
    return found


def registry(root):
    """The codes the registry names, as {code: line number}."""
    codes = {}
    path = root / CODES_FILE
    if not path.exists():
        return codes
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        codes[line.split()[0]] = number
    return codes


def ceilings(root):
    """The per-file ceiling of uncoded report calls, as {relative path: count}."""
    limits = {}
    path = root / BASELINE_FILE
    if not path.exists():
        return limits
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        limits[parts[0]] = int(parts[1])
    return limits


def check_codes(root):
    """Codes used vs codes registered, and the ratchet on reports that state no code."""
    findings = 0
    known = registry(root)
    limits = ceilings(root)
    used = {}
    uncoded = {}
    sources = sorted(root.glob("src/**/*.cpp")) + sorted(root.glob("src/**/*.hpp"))
    if not sources:
        print("src/**/*.{cpp,hpp} matches no file - the code check would silently cover nothing")
        return 1
    for path in sources:
        text = path.read_text(encoding="utf-8", errors="ignore")
        relative = path.relative_to(root).as_posix()
        missing = 0
        for name in REPORT_NAMES:
            # A DEFINITION IS NOT A CALL: `RenderDevice::reportDiagnostic(...)` is where the report is made, and it
            # names no cause because it IS the cause's consumer. The positions come from the same expression
            # `split_calls` walks, so the two iterators pair up in order.
            positions = [match.start() for match in re.finditer(r"\b" + re.escape(name) + r"\(", text)]
            for (line, body), start in zip(split_calls(text, name), positions):
                if start >= 2 and text[start - 2:start] == "::":
                    continue
                codes = CODE_RE.findall(body)
                for code in codes:
                    used.setdefault(code, []).append(f"{relative}:{line}")
                if not codes:
                    missing += 1
        # A report built field by field states its code as an assignment instead of an argument.
        for match in ASSIGNED_CODE_RE.finditer(text):
            used.setdefault(match.group(1), []).append(f"{relative}:{text[:match.start()].count(chr(10)) + 1}")
        if missing and path.suffix == ".cpp":
            # THE RATCHET IS ABOUT REPORTS, and a header DECLARES them: `virtual void reportRenderTarget(...)` names no
            # cause because it is not where any report is made.
            uncoded[relative] = missing
    for code, places in sorted(used.items()):
        if code not in known:
            findings += 1
            print(f"{places[0]}: code '{code}' is not registered in {CODES_FILE}")
    for code, number in sorted(known.items()):
        if code not in used:
            findings += 1
            print(f"{CODES_FILE}:{number}: code '{code}' is registered but no source states it")
    for relative, count in sorted(uncoded.items()):
        ceiling = limits.get(relative, 0)
        if count > ceiling:
            findings += 1
            print(f"{relative}: {count} report call(s) without a code (ceiling {ceiling}) - a new report names its cause")
    for relative, ceiling in sorted(limits.items()):
        if uncoded.get(relative, 0) < ceiling:
            print(f"hint: {relative} now has {uncoded.get(relative, 0)} uncoded report(s) - lower its ceiling in {BASELINE_FILE}")
    print(f"--- {len(used)} code(s) in use, {len(known)} registered, {sum(uncoded.values())} uncoded report call(s) ratcheted")
    return findings

def main(argv):
    root = pathlib.Path(__file__).resolve().parent.parent
    findings = []
    if argv:
        paths = [pathlib.Path(arg) for arg in argv]
    else:
        paths = []
        for pattern in DEFAULT_GLOBS:
            matched = sorted(root.glob(pattern))
            if not matched:
                findings.append(f"{pattern}: matches no file - the check would silently cover nothing")
            paths.extend(matched)
        paths = sorted(set(paths))
    suspicious = sum(check(path) for path in paths)
    for finding in findings:
        print(finding)
    codes = 0 if argv else check_codes(root)
    total = suspicious + len(findings) + codes
    extra = f", {len(findings)} discovery finding(s)" if findings else ""
    print(f"--- {suspicious} suspicious call(s) in {len(paths)} file(s){extra}")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
