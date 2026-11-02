#!/usr/bin/env python3
"""check_translations.py — report catalogs that no longer match the strings in the code.

A `.ts` is the source of truth for a module's UI text, and it rots in one
direction: someone edits or adds a `tr()` / `translate()` string and does not run
`lupdate`, so the catalog keeps an old source text (the new one falls back to
Chinese in an English UI) or keeps an entry nobody says any more (P1 in
.ai/design/appfw-translation.md). Neither is a compile error, and neither is
visible in review, so the drift accumulates until somebody switches the UI to
English and finds half of it untranslated.

How it decides: for every `vn_add_translations()` call it takes the module's own
sources, runs the real `lupdate` over a COPY of the tracked `.ts`, and compares
the two catalogs as (context, source) sets plus the finished state of each
translation. Working on a copy is what makes this a check instead of a rewrite:
`lupdate` merges into the file it is given, so the copy keeps the existing
translations and only the differences show up.

Comparing parsed content rather than bytes is deliberate. `lupdate` re-escapes
text (`'` becomes `&apos;`) and its formatting moves between Qt versions; a byte
diff would fire on that instead of on the thing this check exists for.

What it reports:
  * missing      — a source string in the code that the catalog does not have;
  * obsolete     — a catalog entry no source string produces any more;
  * unfinished   — an entry with no English translation (an empty or
                   `type="unfinished"` element), which silently falls back to
                   Chinese in an English UI;
  * no catalog   — a module with translatable strings but no vn_add_translations()
                   call, whose text therefore never reaches a catalog at all. That
                   case is why the check looks at every module, not only at the
                   ones that already have a `.ts`.

What it does not decide: whether a string *should* be translated. Diagnostics
(logs, exception text) must stay English and must not go through tr() (N3), and
that is review's call, not a script's.

Only catalogs under `src/` are checked: those are the ones that ship. The test
fixture catalogs in `tests/` are pinned by the tests that read them, and scanning
a test directory would pick up every fixture's strings as if they belonged to one
catalog.

Usage: python3 scripts/check_translations.py [--lupdate PATH]
Exit status: 1 when a catalog disagrees with its sources, 0 when they agree.
Without `lupdate` on this machine the check prints a warning and passes — the same
trade-off vn_add_translations() makes for lrelease (P6).
"""
from __future__ import annotations

import argparse
import glob
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCAN_ROOT = ROOT / "src"

ADD_TRANSLATIONS = re.compile(r"vn_add_translations\s*\(([^)]*)\)", re.DOTALL)


def find_lupdate(explicit: str | None) -> str | None:
    """Returns the lupdate executable to use, or None when the machine has none."""
    if explicit:
        return explicit if pathlib.Path(explicit).exists() else None
    override = os.environ.get("LUPDATE")
    if override and pathlib.Path(override).exists():
        return override
    for name in ("lupdate", "lupdate-qt6", "lupdate6"):
        found = shutil.which(name)
        if found:
            return found
    # Debian/Ubuntu keep Qt's tools in the Qt bin directory and do not put it on PATH.
    for pattern in ("/usr/lib/qt6/bin/lupdate", "/usr/lib/*/qt6/bin/lupdate"):
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def catalogs() -> list[tuple[pathlib.Path, str, pathlib.Path]]:
    """Finds every vn_add_translations() call under src/.

    @return (module directory, context, .ts file) for each catalog the build embeds.
    """
    found = []
    for cmake_file in sorted(SCAN_ROOT.rglob("CMakeLists.txt")):
        text = cmake_file.read_text(encoding="utf-8")
        for body in ADD_TRANSLATIONS.findall(text):
            context = re.search(r"CONTEXT\s+(\S+)", body)
            ts_files = re.search(r"TS_FILES\s+(.+?)(?:\s+[A-Z_]+\s+|$)", body, re.DOTALL)
            if not context or not ts_files:
                continue
            module = cmake_file.parent
            for raw in ts_files.group(1).split():
                ts_file = module / raw
                if ts_file.suffix == ".ts":
                    found.append((module, context.group(1), ts_file))
    return found


def module_dirs() -> list[pathlib.Path]:
    """Finds the modules under src/: a CMakeLists.txt that has no child of its own.

    The aggregators (src/, src/fw/, src/plugins/, ...) also carry a CMakeLists.txt; they describe their children and
    have no code of their own, so scanning them would report every string twice.

    @return Module directories, deepest first.
    """
    cmake_files = sorted(SCAN_ROOT.rglob("CMakeLists.txt"))
    dirs = [path.parent for path in cmake_files]
    aggregates = {parent for parent in dirs if any(other != parent and parent in other.parents for other in dirs)}
    return [path for path in dirs if path not in aggregates]


def source_dirs(module: pathlib.Path) -> list[pathlib.Path]:
    """Directories to hand to lupdate: a module's own code, never its generated or vendored parts."""
    dirs = [module / name for name in ("src", "sdk") if (module / name).is_dir()]
    return dirs or [module]


def parse(ts_file: pathlib.Path) -> dict[str, dict[str, str | None]]:
    """Reads a .ts into {context: {source: translation}}, with None for an unfinished translation."""
    tree = ET.parse(ts_file)
    catalogs: dict[str, dict[str, str | None]] = {}
    for context in tree.getroot().findall("context"):
        name = context.findtext("name", default="")
        entries: dict[str, str | None] = {}
        for message in context.findall("message"):
            source = message.findtext("source", default="")
            translation = message.find("translation")
            text = (translation.text or "") if translation is not None else ""
            unfinished = translation is None or translation.get("type") == "unfinished" or not text
            entries[source] = None if unfinished else text
        catalogs[name] = entries
    return catalogs


def compare(module: pathlib.Path, context: str, ts_file: pathlib.Path, lupdate: str) -> list[str]:
    """Runs lupdate over a copy of @p ts_file and reports how the two catalogs disagree."""
    with tempfile.TemporaryDirectory() as temp_dir:
        copy = pathlib.Path(temp_dir) / ts_file.name
        shutil.copyfile(ts_file, copy)
        command = [lupdate, "-locations", "none", "-no-obsolete", *map(str, source_dirs(module)), "-ts", str(copy)]
        result = subprocess.run(command, capture_output=True, text=True, cwd=str(ROOT))
        if not copy.exists() or result.returncode != 0:
            output = (result.stderr or result.stdout).strip()
            return [f"{ts_file}: lupdate failed: {output.splitlines()[-1] if output else 'no output'}"]

        expected = parse(copy)
        tracked = parse(ts_file)

    fix = f"run: {' '.join(command[:-1])} {ts_file.relative_to(ROOT)}"
    findings = []
    for name, entries in expected.items():
        if name != context:
            findings.append(f"{ts_file}: lupdate produced context '{name}'; the build embeds '{context}' ({fix})")
        known = tracked.get(name, {})
        for source, translation in entries.items():
            if source not in known:
                findings.append(f"{ts_file}: missing source {source!r} in context '{name}' ({fix})")
            elif translation is None:
                findings.append(f"{ts_file}: no English translation for {source!r} in context '{name}'")
    for name, entries in tracked.items():
        produced = expected.get(name, {})
        for source in entries:
            if source not in produced:
                findings.append(f"{ts_file}: obsolete source {source!r} in context '{name}' ({fix})")
    return findings


def orphaned_text(module: pathlib.Path, lupdate: str) -> str | None:
    """Reports a module that has translatable strings but no catalog to put them in."""
    with tempfile.TemporaryDirectory() as temp_dir:
        probe = pathlib.Path(temp_dir) / f"{module.name}.ts"
        command = [lupdate, "-locations", "none", *map(str, source_dirs(module)), "-ts", str(probe)]
        result = subprocess.run(command, capture_output=True, text=True, cwd=str(ROOT))
        if not probe.exists() or result.returncode != 0:
            output = (result.stderr or result.stdout).strip()
            return f"{module.relative_to(ROOT)}: lupdate failed: {output.splitlines()[-1] if output else 'no output'}"
        produced = parse(probe)

    count = sum(len(entries) for entries in produced.values())
    if count == 0:
        return None
    contexts = ", ".join(sorted(produced))
    return (f"{module.relative_to(ROOT)}: {count} translatable string(s) in context(s) {contexts} but no "
            f"vn_add_translations() call - they are never translated")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--lupdate", help="lupdate executable to use (default: search PATH and the Qt bin directory)")
    arguments = parser.parse_args()

    lupdate = find_lupdate(arguments.lupdate)
    if lupdate is None:
        print("--- warning: lupdate not found, the catalogs were not checked (see scripts/check_translations.py)")
        return 0

    modules = catalogs()
    findings = []
    for module, context, ts_file in modules:
        if not ts_file.exists():
            findings.append(f"{ts_file}: named by vn_add_translations() but missing on disk")
            continue
        findings.extend(compare(module, context, ts_file, lupdate))

    catalogued = {module for module, _context, _ts in modules}
    checked = 0
    for module in module_dirs():
        if module in catalogued:
            continue
        checked += 1
        orphan = orphaned_text(module, lupdate)
        if orphan:
            findings.append(orphan)

    for finding in findings:
        print(finding)
    print(f"--- {len(findings)} translation finding(s) in {len(modules)} catalog(s), {checked} module(s) without one")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
