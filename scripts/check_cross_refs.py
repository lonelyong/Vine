#!/usr/bin/env python3
"""Checks that a section reference between documentation pages resolves.

A page says "see `graphics-threading.md` §2.5", a section is renumbered or a chapter is folded away,
and the pointer leads nowhere. Nothing compiles a `§` reference, and a reader following one has no
way to tell a live pointer from a dead one, so the drift only surfaces when someone is looking for
an answer and finds a hole (the review that added this gate found four: `async-design.md` §4.4,
`base-crash.md` §2.10, `graphics-threading.md` §3.1.6 and `graphics-architecture.md` §8.2).

What is decided exactly, and therefore gated:

  A reference is a documentation page's file name followed by a section number, written
  ``name.md §2.5``, ``name.md 2.5``, ``name.md` §2.5`` or ``../design/name.md §8.2``. The number must
  either be introduced by ``§`` or contain a dot - that is what keeps a sentence such as
  "`graphics.md` 1653 lines" from being read as a reference to section 1653.

  The reference resolves when a page with that base name, among the pages that carry numbered
  headings (``.ai/design/**`` and ``docs/guides/**``), has that section. A page with no numbered
  headings (an ADR, a memory note, a research page) is not a target: those pages do not promise a
  section numbering, so a number after their name is prose.

What is skipped, on purpose:
  * a section number with no file name beside it (a reference within the page itself) - the reader
    of a page can see its own headings;
  * images, links and code, because the reference is matched in prose text only;
  * a line carrying ``<!-- drift-ok -->`` - the same escape hatch ``check_doc_symbols.py`` uses, for a
    sentence that has to name a reference that no longer resolves (a page recording a repair, for
    instance).

Exit code 1 with a list of findings, 0 when every reference resolves.

Usage:
    python3 scripts/check_cross_refs.py
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Where references may APPEAR. The whole AI corpus and the user guides, because both are read by
# people who follow pointers.
SOURCE_GLOBS = (".ai/**/*.md", "docs/guides/**/*.md")
# Pages that may be the TARGET of a reference: the pages that have a section numbering. A `.md` name
# that resolves to none of these is prose, not a reference (see the module note).
TARGET_GLOBS = (".ai/design/**/*.md", "docs/guides/**/*.md")

# `name.md` + optional `§` + a section number. The backtick is optional on both sides because both
# spellings are used in the corpus ("`x.md` §2.9" and "x.md §2.9").
REFERENCE = re.compile(r"([A-Za-z0-9_.\-]+\.md)`?\s*(§\s*)?(\d+(?:\.\d+)*)")
# The repo-wide escape hatch for a line that must name something absent (see the module note).
DRIFT_OK_MARKER = "<!-- drift-ok -->"


def numbered_sections(path: pathlib.Path) -> set[str]:
    """Returns the section numbers one page declares (chapters, subsections, design points)."""
    text = path.read_text(encoding="utf-8", errors="ignore")
    found: set[str] = set()
    # `## 3. Features`, `### 3.1 Device`, `#### 3.2.1 ...`
    found.update(m.group(1) for m in re.finditer(r"^##+ (\d+(?:\.\d+)*)[.\s]", text, re.MULTILINE))
    # `2.9 **A design point**` - the design-point numbering is a section number for citation purposes.
    found.update(f"{m.group(1)}.{m.group(2)}" for m in re.finditer(r"^(\d+)\.(\d+)\s+\*\*", text, re.MULTILINE))
    return found


def targets() -> dict[str, list[pathlib.Path]]:
    """Returns base name -> pages carrying a section numbering (a name may exist in more than one folder)."""
    index: dict[str, list[pathlib.Path]] = {}
    for pattern in TARGET_GLOBS:
        for path in sorted(ROOT.glob(pattern)):
            index.setdefault(path.name, []).append(path)
    return index


def findings() -> list[str]:
    """Returns one line per reference that resolves to nothing."""
    index = targets()
    anchored = {path: numbered_sections(path) for paths in index.values() for path in paths}

    out: list[str] = []
    for pattern in SOURCE_GLOBS:
        for page in sorted(ROOT.glob(pattern)):
            text = page.read_text(encoding="utf-8", errors="ignore")
            for match in REFERENCE.finditer(text):
                # The escape hatch may sit on any line the reference SPANS: a wrapped reference
                # ("`architecture.md`\n §8.2") is one claim, and the author cannot know which line a
                # parser will pick.
                span_start = text.rfind("\n", 0, match.start()) + 1
                span_end = text.find("\n", match.end())
                span_end = len(text) if span_end == -1 else span_end
                if DRIFT_OK_MARKER in text[span_start:span_end]:
                    continue
                name = match.group(1).split("/")[-1]
                has_marker = match.group(2) is not None
                section = match.group(3)
                if not has_marker and "." not in section:
                    continue  # prose ("`graphics.md` 1653 lines"), not a section reference
                candidates = index.get(name)
                if candidates is None:
                    continue  # a page outside the numbered corpus: prose, not a reference
                if any(section in anchored[path] for path in candidates):
                    continue
                line = text[: match.start()].count("\n") + 1
                out.append(
                    f"{page.relative_to(ROOT)}:{line}: {name} has no section {section}"
                    f" (see {', '.join(str(p.relative_to(ROOT)) for p in candidates)})"
                )
    return out


def main() -> int:
    problems = findings()
    for problem in problems:
        print(problem)
    if problems:
        print(f"\n{len(problems)} cross-reference(s) lead nowhere.")
        return 1
    print("cross-references: every section reference resolves")
    return 0


if __name__ == "__main__":
    sys.exit(main())
