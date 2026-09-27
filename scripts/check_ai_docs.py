#!/usr/bin/env python3
"""Keep `.ai/` usable as a source of truth: the index knows the tree, and the notes point at things that exist.

`.ai/` is repository knowledge an assistant and a human read to decide what the code IS (see
`.ai/README.md`). Two kinds of rot are visible from outside, and both were present on 2026-09-27 - the
index named 15 of 62 documents, and 175 references in the notes' CURRENT sections named units and paths
that no longer existed (the vsg rewrite renamed whole layers and the notes kept the old names):

  R1 (always fails)  Every `.ai/**/*.md` must be named in `.ai/README.md`. An unindexed note is a note
                     nobody finds - and the count only grows, because adding a file is easy.
  R2 (always fails)  Every `*.md` the index names must exist. The index itself rots the same way; a
                     pointer to a deleted note is worse than no entry, because it looks like coverage.
  R3 (reports, `--strict` fails)  In a section that is NOT marked as history, every backticked unit name
                     (`Foo.hpp` / `Foo.cpp`), every repository path and every document name
                     (`graphics-layering.md`) must resolve. A sentence in a current section that names a
                     deleted unit or a renamed document is a reader following a pointer that leads
                     nowhere - the drift `check_doc_symbols.py` already gates for the plugin's three
                     living documents, which this extends to the whole `.ai/` tree.

R3 reports rather than fails while the backlog it found is being worked through (175 findings on
2026-09-27); `--strict` is what the gate should run once the list is empty. R1/R2 are cheap and true
today, so they gate immediately.

WHAT IS SKIPPED, ON PURPOSE (one convention, shared with `check_doc_symbols.py`):
  * fenced code blocks - a snippet is not a claim about this tree;
  * a section whose HEADING carries a history marker (`历史登记`, `不再更新`, `历史）`, `已废弃`,
    `历史沿革`) - recording what the code used to be is exactly its job, so deleted names are expected
    there. This is why "document the decision, then mark the section" is the way to retire a note.
  * a line carrying `<!-- drift-ok -->` - the escape hatch for a sentence that must name something
    absent (explaining a rejected design, say).

Usage:
    python3 scripts/check_ai_docs.py [--strict]
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
AI = ROOT / ".ai"
INDEX = AI / "README.md"

HISTORY_MARKERS = ("历史登记", "不再更新", "历史）", "已废弃", "历史沿革")
DRIFT_OK = "<!-- drift-ok -->"
FENCE = re.compile(r"^\s*(?:```|~~~)")
HEADING = re.compile(r"^#{1,6}\s")
# A unit is a name a reader can open. Prose may name a symbol that is deliberately absent, so only the
# two shapes a script can decide exactly are looked for.
UNIT_RE = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*\.(?:hpp|cpp))`")
# A note may name another document by bare name (``见 `graphics-layering.md` ``) or by path; both are
# claims about this tree, and both break the same way when a document is renamed.
DOC_RE = re.compile(r"`([A-Za-z0-9._-]+\.md)`")
# A path is a claim about THIS repository: it starts at a known root. `add/remove` in prose is a pair of
# words, not a path (counting those was the first, wrong, measurement of this backlog).
PATH_RE = re.compile(r"`((?:src|tests|tools|scripts|cmake|docs|\.ai)/[A-Za-z0-9_./-]+)`")
INDEX_MD_RE = re.compile(r"`([A-Za-z0-9._-]+\.md)`")
# The roots that may define a "this exists somewhere" unit. Recursive, like check_doc_symbols.py: the
# rewrite moved whole layers, and a top-level walk would have missed the new ones.
UNIT_ROOTS = ("src", "tests", "tools", "cmake")


def notes() -> list[pathlib.Path]:
    """Every note under `.ai/`, the index itself excluded."""
    return sorted(p for p in AI.rglob("*.md") if p != INDEX)


def existing_units() -> set[str]:
    """Every unit file name this repository has, vendored dependencies included."""
    ours = {p.name for base in UNIT_ROOTS for p in (ROOT / base).rglob("*") if p.is_file()}
    # A note may name an upstream file by bare name (`RenderGraph.cpp`). The vendored sources are
    # discovered instead of listed, so this follows whatever the tree actually vendors; a checkout
    # without build trees simply has fewer exemptions.
    vendored = {
        p.name
        for deps in ROOT.glob("build*/_deps")
        for tree in deps.glob("*-src")
        for p in tree.rglob("*")
        if p.suffix in (".hpp", ".cpp")
    }
    return ours | vendored


def existing_docs() -> set[str]:
    """Every markdown document this repository has, by bare name."""
    names = {p.name for p in AI.rglob("*.md")}
    names |= {p.name for p in ROOT.glob("*.md")}
    # `.github/` holds the coding guidelines a note may cite; scripts/cmake/tests hold tooling notes.
    for base in UNIT_ROOTS + ("scripts", "cmake", ".github"):
        names |= {p.name for p in (ROOT / base).rglob("*.md")}
    return names


def existing_paths() -> set[str]:
    """Every path this repository has, as repo-relative strings."""
    return {str(p) for base in UNIT_ROOTS + ("scripts", "cmake", "docs") for p in (ROOT / base).rglob("*")}


def resolves(token: str, paths: set[str]) -> bool:
    """Return whether @p token names something, allowing the spellings a note may use for a unit.

    A note may write `Foo.hpp`, `Foo` or a directory: the first two are the same file, and a directory
    is named without its trailing slash. Extensions are tried because a sentence about `api/ContentPass`
    is a sentence about `api/ContentPass.cpp`.
    """
    if (ROOT / token).exists():
        return True
    # A note may write `Foo.hpp/.cpp` - two files under one token. Expanding the shorthand here is what
    # keeps it usable, and a name that is gone in BOTH forms is still a finding.
    for shorthand in (".hpp/.cpp", ".h/.cpp", ".hpp/.h"):
        if token.endswith(shorthand):
            stem = token[: -len(shorthand)]
            return all(resolves(f"{stem}.{ext}", paths) for ext in ("hpp", "cpp"))
    # The vendored vsg tree has its own `src/vsg/...` layout, and a note may name a file in it
    # (`src/vsg/app/RenderGraph.cpp` is where the upstream calls `vkCmdBeginRenderPass`). That is a
    # statement about the dependency, not drift in our notes.
    if token.startswith("src/vsg/"):
        return True
    return any(
        any(p.endswith("/" + token + suffix) or p == token + suffix for p in paths)
        for suffix in ("", ".hpp", ".cpp", ".md", "/")
    )


def current_sections(path: pathlib.Path):
    """Yield (line number, line) for the lines of @p path that are NOT marked as history.

    A document whose TITLE carries a history marker is a record of what the code used to be (`# Bug: …
    （历史登记）`), so ALL of it is exempt: the names it keeps are the point. Otherwise the exemption
    follows the sections, so marking one heading retires exactly that heading's content.
    """
    text = path.read_text(encoding="utf-8", errors="ignore").split("\n")
    title = next((line for line in text if HEADING.match(line)), "")
    if any(marker in title for marker in HISTORY_MARKERS):
        return
    section = ""
    fenced = False
    for number, line in enumerate(text, start=1):
        if FENCE.match(line):
            fenced = not fenced
            continue
        if fenced:
            continue
        if HEADING.match(line):
            section = line
        if any(marker in section for marker in HISTORY_MARKERS) or DRIFT_OK in line:
            continue
        yield number, line


def main() -> int:
    """Run the three rules and print the findings, one per line, then the summary line."""
    strict = "--strict" in sys.argv[1:]
    index_text = INDEX.read_text(encoding="utf-8")
    notes_found = notes()
    # A note is named by its FILE name, not by its path: the index groups them under `design/`,
    # `memory/` and `bugs/` headings, and a second spelling of the same path in two places would be
    # the drift this script exists to catch. So `name` -> "any note with this name".
    note_names = {note.name for note in notes_found}

    findings = []
    # R1: an unindexed note is a note nobody finds.
    for note in notes_found:
        if note.name not in index_text:
            findings.append(f"{note.relative_to(ROOT)}: not named in .ai/README.md (add it to the index)")
    # R2: the index may not point at notes that are gone.
    for name in sorted(set(INDEX_MD_RE.findall(index_text))):
        if name not in note_names:
            findings.append(f".ai/README.md: names `{name}`, which does not exist")

    units, paths = existing_units(), existing_paths()
    docs = existing_docs()
    references = []
    for note in notes_found:
        for number, line in current_sections(note):
            for match in UNIT_RE.finditer(line):
                if match.group(1) not in units:
                    references.append(
                        f"{note.relative_to(ROOT)}:{number}: `{match.group(1)}` does not exist "
                        f"(fix the name, mark the section 历史登记, or add <!-- drift-ok -->)")
            for match in DOC_RE.finditer(line):
                if match.group(1) not in docs:
                    references.append(
                        f"{note.relative_to(ROOT)}:{number}: `{match.group(1)}` names no document "
                        f"(fix the name, mark the section 历史登记, or add <!-- drift-ok -->)")
            for match in PATH_RE.finditer(line):
                token = match.group(1).rstrip("/")
                if not resolves(token, paths):
                    references.append(
                        f"{note.relative_to(ROOT)}:{number}: `{token}` does not resolve "
                        f"(fix the path, mark the section 历史登记, or add <!-- drift-ok -->)")

    for line in findings + (references if strict else []):
        print(line)
    if not strict and references:
        print(f"(--strict would also report {len(references)} stale reference(s) in current sections)")
    total = len(findings) + (len(references) if strict else 0)
    print(f"--- {total} ai-doc finding(s) in {len(notes_found)} note(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
