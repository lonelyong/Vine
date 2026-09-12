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
  * V_LOGI / V_LOGW / V_LOGE / V_LOGD(...) -> {} placeholders vs top-level commas

Usage: python3 scripts/check_diagnostic_formats.py [files...]
       (defaults to the VSG backend plugin's sources and its selftest)
Exit status: 1 when something is suspicious, so it can gate a build.
"""
import pathlib
import re
import sys

SPEC = re.compile(r"%[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|z|j|t|L)?[diuoxXfFeEgGaAcspn]")
PLACEHOLDER = re.compile(r"\{[^}]*\}")
MACROS = ("V_LOGI", "V_LOGW", "V_LOGE", "V_LOGD")
DEFAULT_GLOBS = (
    "src/plugins/gfx_backend_vsg/src/*.cpp",
    "src/plugins/gfx_backend_vsg/vsg_selftest/*.cpp",
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


def main(argv):
    if argv:
        paths = [pathlib.Path(arg) for arg in argv]
    else:
        root = pathlib.Path(__file__).resolve().parent.parent
        paths = [path for pattern in DEFAULT_GLOBS for path in sorted(root.glob(pattern))]
    suspicious = sum(check(path) for path in paths)
    print(f"--- {suspicious} suspicious call(s) in {len(paths)} file(s)")
    return 1 if suspicious else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
