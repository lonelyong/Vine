#!/usr/bin/env python3
"""Save UTF-8 C++/text files with a BOM - git-tracked only (idempotent).

Why: MSVC interprets a UTF-8 file *without* a BOM using the system ANSI code
page (e.g. 936 on zh-CN Windows), so any Chinese / non-ASCII comment triggers
C4819. A UTF-8 BOM makes MSVC treat the file as UTF-8 unconditionally (the
same effect as /utf-8, but for every tool that reads the file).

Note on coverage: the project's own targets already pass /utf-8
(cmake/VineLibraryHelper.cmake, cmake/VinePluginHelper.cmake), so C4819 cannot
fire in a Vine build either way - this script is belt-and-suspenders for the
tools that ignore the flag (editors, scripts, foreign build systems). The
convention is therefore not uniformly applied (measured 2026-09-25: 306 of 794
tracked non-third_party C++ files carry no BOM, mostly the rewrite-era backend
and its tests); run this script when adopting the convention in a tree, not to
fix a build.

Scope & safety:
  * Processes ONLY files tracked by git (git ls-files); untracked or ignored
    artifacts (build/, dist/, etc.) are never touched.
  * Converted types: C/C++ sources & headers, plus plain text - markdown,
    txt, shell, python.
  * Never touched:
      - third_party/ subtrees (vendored upstream code);
      - CMakeLists.txt, *.cmake, Makefile, *.mk (BOM can trip build tooling);
      - shader sources (*.vert / *.frag / *.glsl / *.comp / ...) - a BOM can
        break glslang's #version handling;
      - any file whose first line is a shebang (#!) - a BOM there breaks
        direct execution, so executable scripts stay byte-identical;
      - binaries / files that are not valid UTF-8 (they are reported, never
        mangled).
  * Idempotent: files that already start with EF BB BF are left untouched.

Usage:
  python3 scripts/utf8_bom.py [prefix...] [--dry-run] [--quiet]
  python3 scripts/utf8_bom.py --dry-run        # preview (whole repo)
  python3 scripts/utf8_bom.py src/ scripts/    # only these prefixes
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

# Extensions eligible for the UTF-8 BOM conversion.
TEXT_EXTS = {
    # C / C++
    ".c", ".cc", ".cpp", ".cxx",
    ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp",
    ".m", ".mm", ".cu",
    # Plain text: docs, scripts, config notes.
    ".md", ".markdown", ".txt", ".sh", ".py",
}
# Extensions never touched regardless of how they decode.
SKIP_EXTS = {
    # binaries / assets
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".tga", ".webp",
    ".so", ".dll", ".dylib", ".lib", ".a", ".o", ".obj", ".exe",
    ".bin", ".dat", ".zip", ".gz", ".7z", ".tar", ".ttf", ".otf",
    ".woff", ".woff2", ".pdf", ".pdb", ".db", ".sqlite", ".wasm", ".qm",
    # raw shader sources (BOM can break glslang #version)
    ".glsl", ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese", ".rgen",
    ".rchit", ".rmiss", ".spv",
    # build-system files whose parsers may not strip a BOM
    ".cmake",
}
SKIP_NAMES = {"CMakeLists.txt", "Makefile"}
SKIP_DIR_PARTS = {"third_party"}

BOM = b"\xef\xbb\xbf"


def repo_root() -> str:
    """Returns the repository root (dir above the script's scripts/ dir)."""
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    if os.path.isdir(os.path.join(root, ".git")) or os.path.isfile(os.path.join(root, ".git")):
        return root
    # Fall back to the first ancestor with a .git entry.
    cur = here
    while True:
        parent = os.path.dirname(cur)
        if parent == cur:
            return here
        if os.path.isdir(os.path.join(cur, ".git")) or os.path.isfile(os.path.join(cur, ".git")):
            return cur
        cur = parent


def tracked_files(root: str) -> list[str]:
    """Returns absolute paths of git-tracked files (whole repo)."""
    try:
        proc = subprocess.run(
            ["git", "-C", root, "ls-files", "-z"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        sys.stderr.write(f"[error] cannot run git: {exc}\n")
        return []
    if proc.returncode != 0:
        sys.stderr.write(f"[error] git ls-files failed: {proc.stderr.decode(errors='replace').strip()}\n")
        return []
    out = proc.stdout.decode("utf-8", errors="replace")
    return [os.path.normpath(os.path.join(root, p)) for p in out.split("\0") if p]


def is_text(data: bytes) -> bool:
    if b"\x00" in data:
        return False
    try:
        data.decode("utf-8")
        return True
    except UnicodeDecodeError:
        return False


def has_shebang(data: bytes) -> bool:
    """True when the first non-BOM bytes are a #! line (skip BOM then)."""
    stripped = data[len(BOM):] if data.startswith(BOM) else data
    return stripped.startswith(b"#!")


def convert(path: str, dry_run: bool, quiet: bool) -> bool:
    """Adds a UTF-8 BOM. Returns True when the file would change / changed."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        sys.stderr.write(f"[skip] cannot read: {path}: {exc}\n")
        return False

    if data.startswith(BOM):
        return False
    if not is_text(data):
        sys.stderr.write(f"[skip] not valid UTF-8 text: {path}\n")
        return False
    if has_shebang(data):
        sys.stderr.write(f"[skip] shebang script (no BOM): {path}\n")
        return False

    if dry_run:
        if not quiet:
            print(f"would rewrite: {path}")
        return True

    try:
        with open(path, "wb") as fh:
            fh.write(BOM + data)
    except OSError as exc:
        sys.stderr.write(f"[error] cannot write: {path}: {exc}\n")
        return False
    if not quiet:
        print(f"rewrote: {path}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Add a UTF-8 BOM to git-tracked C++/text files (idempotent)."
    )
    parser.add_argument("prefixes", nargs="*", default=None,
                        help="only files whose repo-relative path starts with one of these prefixes "
                             "(default: the whole git work tree)")
    parser.add_argument("--dry-run", "-n", action="store_true",
                        help="only list files that would be rewritten")
    parser.add_argument("--quiet", "-q", action="store_true",
                        help="only print the summary line")
    args = parser.parse_args()

    root = repo_root()
    changed = skipped = 0
    for path in tracked_files(root):
        rel = os.path.relpath(path, root).replace(os.sep, "/")
        if rel == ".":
            continue
        parts = rel.split("/")
        # Skip vendored / forbidden directories regardless of extension rules.
        if any(p in SKIP_DIR_PARTS for p in parts):
            continue
        if args.prefixes and not any(rel.startswith(p) for p in args.prefixes):
            continue
        name = os.path.basename(path)
        ext = os.path.splitext(name)[1].lower()
        if name in SKIP_NAMES or ext in SKIP_EXTS or ext not in TEXT_EXTS:
            continue
        try:
            if convert(path, args.dry_run, args.quiet):
                changed += 1
            else:
                skipped += 1
        except OSError as exc:
            sys.stderr.write(f"[skip] {path}: {exc}\n")
            skipped += 1

    mode = "dry-run: would rewrite" if args.dry_run else "rewrote"
    print(f"{mode} {changed} file(s); {skipped} left unchanged/skipped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

