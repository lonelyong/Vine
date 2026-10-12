#!/usr/bin/env python3
"""Checks that every declaration in the public SDK headers documents itself.

WHY. `.github/copilot-instructions.md` asks every public function for `@brief`, one `@param`
per parameter and a `@return` for every non-void return, and nothing checked it. The result is
one-directional drift by module age: the newest surface (graphics) follows the rule almost
everywhere - 2 undocumented groups out of 76 headers - while the oldest (base) documents its
classes and leaves the members. A rule that only a reviewer remembers is a rule that rots, so
this gate pins today's tree as a **baseline** and fails when a *new* undocumented declaration
shows up. It is a floor, not a cleanup: the baseline shrinks whenever someone documents
something (`--update` records that).

WHAT IS CHECKED, exactly (so the numbers mean something):

  * only `src/**/sdk/**/*.hpp` - the public SDK headers - and only at class or file scope: a
    declaration inside a function body, or inside a `detail` namespace/class, is not part of
    the surface and is skipped;
  * a declaration inherits the doc block of the group above it (consecutive declarations with
    no blank line and no access specifier in between), because the house style documents
    accessor bundles with one block;
  * `@brief` is required for every declaration that declares a function;
  * `@param` is required for every parameter (balanced-paren count: a trailing `requires(...)`
    clause or an inline body is not a parameter list);
  * `@return` is required unless the return type is `void`;
  * exempt, on purpose: coroutine plumbing (`await_ready` / `await_suspend` / `await_resume` /
    `await_transform` / `promise_type` / `return_value` / `return_void` / `yield_value` /
    `unhandled_exception` / `get_return_object` / `initial_suspend` / `final_suspend` - these
    are for the compiler, not for a reader), deleted/defaulted special members, and
    `operator` overloads.

THE BASELINE (`scripts/public_docs_baseline.txt`) lists what the tree already owes: one
`path <TAB> kind|Class::name/arity` line per obligation, sorted. The gate fails on any entry the
baseline does not have - so it goes red both when a new undocumented declaration appears and when
documentation is *removed* (the debt grew above the recorded floor). Documenting something only
turns its entries stale (informational) and keeps the run green; `--update` is what lowers the
floor, and it is a line in a commit that a reviewer can read. Carrying the declaration identity -
rather than a count per file - is what keeps "documented one, added another" red.

The three obligations are independent: a declaration that owes `@brief` and `@return` is two
entries, so adding one of them can never surface the other as "new".

SIZE, honestly (2026-10-13, the day it was written): 1164 obligations in 127 headers - `@brief`
454, `@param` 288, `@return` 422; by module appfw 529, core 194, graphics 148, math 110,
robotics/core 50, async 39, geometry 26, logging 24, the rest single digits. It is a floor to stop
the drift, not a cleanup: every `--update` can only make it smaller, and the per-module numbers are
the ordering of what to document first.

KNOWN LIMITS, on purpose: two overloads of the same name and arity in one class share an identity,
so adding a third such overload is not caught; a declaration that moves between classes reads as a
new one.

Usage:
    python3 scripts/check_public_docs.py            # gate (exit 1 on a new finding)
    python3 scripts/check_public_docs.py --update   # rewrite the baseline from the tree
    python3 scripts/check_public_docs.py --list      # print every current finding
    python3 scripts/check_public_docs.py --why <path>  # explain how one header was parsed
"""

from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE = Path(__file__).resolve().parent / "public_docs_baseline.txt"

SPEC = r"(?:virtual\s+|static\s+|inline\s+|constexpr\s+|explicit\s+|friend\s+|template\s*<[^>]*>\s*)*"
HEAD_RE = re.compile(SPEC + r"(?P<ret>[A-Za-z_][\w:<>,\s\*&]*?)\s+(?P<name>[A-Za-z_]\w*)\s*$")
CTOR_RE = re.compile(SPEC + r"(?P<name>[A-Za-z_]\w*)\s*$")
CLASS_OPEN_RE = re.compile(r"^(class|struct|union)\b")
ACCESS_RE = re.compile(r"^(public|private|protected)\s*:")
NAME_RE = re.compile(r"^[A-Za-z_]\w*$")
SPEC_ONLY_RE = re.compile(
    r"^(\[\[.*\]\]|constexpr|consteval|inline|static|virtual|explicit|noexcept|friend|typename|nodiscard)$"
)
SPECIFIER_AS_TYPE = {"explicit", "virtual", "static", "inline", "constexpr", "friend", "template", "typename"}

PLUMBING = {
    "await_ready", "await_suspend", "await_resume", "await_transform", "promise_type",
    "return_value", "return_void", "yield_value", "unhandled_exception", "get_return_object",
    "initial_suspend", "final_suspend",
}
RESERVED = {"if", "for", "while", "switch", "return", "catch", "sizeof", "static_assert",
            "co_return", "else", "namespace", "using", "typedef", "operator"}


def strip_comments(text: str) -> list[str]:
    """Blanks comments while keeping every line index usable against the raw lines."""
    without_block = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return [re.sub(r"//.*$", "", line) for line in without_block.splitlines()]


def split_params(text: str) -> list[str]:
    if not text.strip() or text.strip() == "void":
        return []
    out, depth, current = [], 0, ""
    for char in text:
        if char in "(<[{":
            depth += 1
        elif char in ")>]}":
            depth -= 1
        if char == "," and depth == 0:
            out.append(current)
            current = ""
        else:
            current += char
    out.append(current)
    return [part for part in (item.strip() for item in out) if part and part != "..."]


def doc_block_above(raw: list[str], line_no: int, ignored: set[int]) -> list[str]:
    """Collects the doc block ending just above `line_no` (skips attributes, allows a blank line)."""
    doc, index, blanks = [], line_no - 2, 0
    while index >= 0:
        if index in ignored:
            index -= 1
            continue
        text = raw[index].strip()
        if not text:
            blanks += 1
            if blanks > 1:
                break
            index -= 1
            continue
        if SPEC_ONLY_RE.match(text):
            index -= 1
            continue
        if text.startswith(("*/", "*", "///", "/**")):
            doc.insert(0, text)
            if text.startswith("/**"):
                break
            index -= 1
            continue
        break
    return doc


def ignorable_lines(lines: list[str]) -> set[int]:
    """Template headers (`template <...>`, possibly multi-line) and `#define` continuations.

    Both sit between a doc block and the declaration it documents; without this the walk above ends
    early and a documented function reads as undocumented (`make_intrusive`, `Char.hpp`'s helpers).
    """
    skipped: set[int] = set()
    angle, in_template = 0, False
    in_macro = False
    for index, text in enumerate(lines):
        stripped = text.strip()
        if in_macro:
            skipped.add(index)
            in_macro = stripped.endswith("\\")
            continue
        if stripped.startswith("#define") and stripped.endswith("\\"):
            skipped.add(index)
            in_macro = True
            continue
        if stripped.startswith("template"):
            in_template = True
            angle = 0
        if in_template:
            skipped.add(index)
            angle += stripped.count("<") - stripped.count(">")
            if angle <= 0:
                in_template = False
    return skipped


def findings_of(path: Path, explain: list[tuple] | None = None) -> list[tuple[int, str, str]]:
    """Returns (line, kind, signature) for every undocumented declaration in one header.

    `explain` (used by `--why`) collects one row per parsed declaration so a disputed finding can
    be traced: (line, identity, arity, returns_void, documented, group_head, doc_lines).
    """
    raw = path.read_text(encoding="utf-8", errors="replace").splitlines()
    lines = strip_comments("\n".join(raw))
    ignored = ignorable_lines(lines)
    findings: list[tuple[int, str, str]] = []

    brace = 0
    scopes: list[tuple[int, str, str, bool]] = []   # (body depth, access, name, skip)
    namespace_depths: list[tuple[int, bool]] = []  # (body depth, is_detail)
    pending_class: tuple[str, str, bool] | None = None
    prev_decl_end: int | None = None
    group_documented = False

    index = 0
    while index < len(lines):
        if index in ignored:
            index += 1
            continue
        line = lines[index].strip()
        if not line:
            prev_decl_end, group_documented = None, False
            index += 1
            continue

        if CLASS_OPEN_RE.match(line) and ";" not in line:
            # The name is the token after the keyword, but an export macro may sit in between
            # (`class VN_CORE_API Exception : public std::exception {`).
            name_tokens = re.findall(r"[A-Za-z_]\w*", re.sub(r"\{.*$", "", line))
            name = ""
            for token in name_tokens[1:]:
                if re.match(r"^[A-Z][A-Z0-9_]*$", token):
                    continue      # VN_CORE_API / VN_GRAPHICS_API / ...
                name = token
                break
            # bool(...) on purpose: `name == "detail" or (scopes and scopes[-1][3])` yields a LIST
            # (the empty list is falsy) and storing it makes later `bool()` reads depend on a shared
            # object - that shipped every member of every class as "skipped" once.
            skip = bool(name == "detail" or (scopes and scopes[-1][3]))
            if "{" in line:
                brace += 1
                scopes.append((brace, "public" if not line.startswith("class") else "private", name, skip))
            else:
                pending_class = (name, "public" if not line.startswith("class") else "private", skip)
            prev_decl_end, group_documented = None, False
            index += 1
            continue

        if line.startswith("namespace"):
            brace += line.count("{") - line.count("}")
            if "{" in line:
                namespace_depths.append((brace, "detail" in line))
            while namespace_depths and brace < namespace_depths[-1][0]:
                namespace_depths.pop()
            prev_decl_end, group_documented = None, False
            index += 1
            continue

        access_match = ACCESS_RE.match(line)
        if access_match:
            if scopes:
                scopes[-1] = (scopes[-1][0], access_match.group(1), scopes[-1][2], scopes[-1][3])
            prev_decl_end, group_documented = None, False
            index += 1
            continue

        at_class_scope = bool(scopes) and brace == scopes[-1][0]
        at_file_scope = not scopes and brace == (namespace_depths[-1][0] if namespace_depths else 0)
        access = scopes[-1][1] if scopes else "public"
        skip_scope = bool(scopes and scopes[-1][3]) or (namespace_depths[-1][1] if namespace_depths else False)

        if (at_class_scope or at_file_scope) and access == "public" and not skip_scope and "(" in line and not line.startswith("#"):
            block, cursor, paren_depth = "", index, 0
            while cursor < len(lines) and cursor - index < 12:
                block = (block + " " + lines[cursor].strip()).strip()
                paren_depth += lines[cursor].count("(") - lines[cursor].count(")")
                if paren_depth <= 0 and block.endswith((";", "{", "}")):
                    break
                cursor += 1
            if cursor < len(lines) and paren_depth == 0 and block.endswith((";", "{", "}")):
                start_line = index + 1
                signature = parse_signature(block, scopes[-1][2] if scopes else "")
                if signature is not None:
                    kind, identity, arity, returns_void = signature
                    group_head = prev_decl_end is None or (start_line - prev_decl_end) > 2
                    doc = doc_block_above(raw, start_line, ignored)
                    documented = any("@brief" in text for text in doc)
                    if group_head:
                        group_documented = documented
                    inherited = documented or group_documented
                    if explain is not None:
                        explain.append((start_line, identity, arity, returns_void, documented,
                                        group_head, len(doc)))
                    if not inherited:
                        findings.append((start_line, "brief", identity))
                    # The three obligations are independent on purpose: documenting a declaration
                    # must never make the gate report a *new* one (the short-circuit version shipped
                    # `brief` only, so adding the @brief surfaced the missing @return as "new" and
                    # turned progress red).
                    params = sum(text.count("@param") for text in doc)
                    if arity and params < arity:
                        findings.append((start_line, "param", f"{identity}/{arity}"))
                    if not returns_void and not any("@return" in text for text in doc):
                        findings.append((start_line, "return", identity))
                    prev_decl_end = start_line

        brace += line.count("{") - line.count("}")
        if pending_class is not None and "{" in line:
            scopes.append((brace, pending_class[1], pending_class[0], pending_class[2]))
            pending_class = None
        while scopes and brace < scopes[-1][0]:
            scopes.pop()
        index += 1

    return findings


def parse_signature(block: str, scope_name: str) -> tuple[str, str, int, bool] | None:
    """(kind, `Scope::name`, arity, returns_void) for a declaration, or None when it is not one."""
    body = block.find("{")
    signature = block if body < 0 else block[:body]
    open_paren = signature.find("(")
    if open_paren < 0:
        return None
    depth, close = 0, -1
    for position in range(open_paren, len(signature)):
        if signature[position] == "(":
            depth += 1
        elif signature[position] == ")":
            depth -= 1
            if depth == 0:
                close = position
                break
    if close < 0:
        return None
    head = signature[:open_paren].strip()
    params = split_params(signature[open_paren + 1 : close])

    head_match, ctor_match = HEAD_RE.match(head), CTOR_RE.match(head)
    if head_match is not None and head_match.group("ret").strip() in SPECIFIER_AS_TYPE:
        head_match = None
    name = head_match.group("name") if head_match else (ctor_match.group("name") if ctor_match else None)
    returns_void = head_match is None or head_match.group("ret").strip() == "void"
    if name is None or name in RESERVED or not NAME_RE.match(name):
        return None
    if name in PLUMBING or "=" in head:
        return None
    if re.match(r"^[A-Z][A-Z0-9_]*$", name):
        return None   # a macro being invoked at class scope (VN_OBJECT_META_DECL, ...)
    if re.search(r"=\s*(delete|default)\b", block):
        return None
    identity = f"{scope_name}::{name}" if scope_name else name
    return ("function", identity, len(params), returns_void)


def sdk_headers() -> list[Path]:
    return sorted(path for path in (ROOT / "src").rglob("*.hpp") if "sdk" in path.parts)


def collect() -> dict[str, set[str]]:
    current: dict[str, set[str]] = {}
    for path in sdk_headers():
        entries = {f"{kind}|{signature}" for _, kind, signature in findings_of(path)}
        if entries:
            current[path.relative_to(ROOT).as_posix()] = entries
    return current


def read_baseline() -> dict[str, set[str]]:
    baseline: dict[str, set[str]] = {}
    if not BASELINE.exists():
        return baseline
    for line in BASELINE.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        path, entry = line.split("\t", 1)
        baseline.setdefault(path, set()).add(entry.strip())
    return baseline


def write_baseline(current: dict[str, set[str]]) -> None:
    lines = ["# Public SDK declarations that still owe documentation - see check_public_docs.py.",
             "# One entry per declaration: <path>\\t<kind>|<Class::name/arity>. Drop lines that no longer apply."]
    for path in sorted(current):
        for entry in sorted(current[path]):
            lines.append(f"{path}\t{entry}")
    BASELINE.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main() -> int:
    if "--why" in sys.argv:
        target = sys.argv[sys.argv.index("--why") + 1]
        explain: list[tuple] = []
        found = findings_of(ROOT / target, explain)
        print(f"-- {target}: {len(explain)} declaration(s) parsed, {len(found)} finding(s)")
        for row in explain:
            line, identity, arity, returns_void, documented, group_head, doc_lines = row
            print(f"   line {line:<5} {identity:<40} arity={arity} void={int(returns_void)} "
                  f"doc={int(documented)} head={int(group_head)} doc_lines={doc_lines}")
        for line, kind, signature in found:
            print(f"   FINDING line {line}: {kind} {signature}")
        return 0

    current = collect()
    if "--update" in sys.argv:
        write_baseline(current)
        total = sum(len(entries) for entries in current.values())
        print(f"--- baseline rewritten: {total} entr(ies) in {len(current)} file(s)")
        return 0

    baseline = read_baseline()
    if "--list" in sys.argv:
        for path in sorted(current):
            print(f"-- {path}")
            for entry in sorted(current[path]):
                print(f"   {'(baseline)' if entry in baseline.get(path, set()) else '** NEW **'} {entry}")
        return 0

    counts, new_entries, stale, listed = Counter(), [], [], []
    for path in sorted(current):
        known = baseline.get(path, set())
        for entry in sorted(current[path]):
            if entry in known:
                counts["baseline"] += 1
            else:
                counts["new"] += 1
                new_entries.append(f"{path}\t{entry}")
        for entry in sorted(known - current[path]):
            counts["stale"] += 1
            stale.append(f"{path}\t{entry}")

    missing_files = [path for path in baseline if path not in current]
    if missing_files:
        counts["stale"] += len(missing_files)
        stale.extend(f"{path}\t(file has no findings any more)" for path in missing_files)

    for entry in new_entries:
        listed.append(entry)
    listed.sort(key=lambda line: list(current).index(line.split("\t")[0]) if line.split("\t")[0] in current else 0)
    for entry in listed:
        print(f"NEW\t{entry}")
    print(f"--- {counts['baseline']} baseline entr(ies), {counts['new']} new, {counts['stale']} stale in "
          f"{len(current)} file(s)")
    if counts["new"]:
        print("--- a public declaration needs @brief / @param (per parameter) / @return (non-void); "
              "see .github/copilot-instructions.md")
        return 1
    if counts["stale"]:
        print("--- stale baseline entr(ies) no longer apply: run --update to drop them")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
