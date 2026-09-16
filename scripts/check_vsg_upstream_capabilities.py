#!/usr/bin/env python3
"""check_vsg_upstream_capabilities.py — keeps the "blocked on vsg" verdicts honest.

Two backlog items are parked with the same reason: vsg offers no way to do it, so
Vine waits for upstream. A parked item whose premise quietly stops being true is
worse than an open one: nobody looks at it again. This script re-reads the PINNED
vsg checkout and reports whether each premise still holds, so a vsg upgrade that
changes the situation fails here instead of staying invisible.

The two claims:

  V3 / D28 -- vsg never uses a VkPipelineCache.
      Every vkCreate*Pipelines call in vsg's own sources passes VK_NULL_HANDLE as
      the cache argument, and no other vsg source (outside the Vulkan headers)
      names the type. Consequence: no pipeline cache can be injected, so PSOs
      cannot be reused across sessions or processes, whatever Vine does.

  V4 -- vsg records with vkCmdBeginRenderPass, never with dynamic rendering.
      vsg's recording path holds no vkCmdBeginRendering, which is what makes "one
      render pass per pass, no merging" something Vine cannot change from outside.

Usage:
    check_vsg_upstream_capabilities.py [vsg-root]

The vsg root is taken from the argument, else $VINE_VSG_SRC_DIR, else the first
build*/_deps/vsg-src that exists. When no vsg checkout is present the check is
SKIPPED and exits 0 (a source-less tree cannot falsify the claims).

Exit codes: 0 = every claim still holds (or skipped), 1 = a claim no longer holds.
"""

from __future__ import annotations

import os
import pathlib
import re
import sys

PIPELINE_CALL = re.compile(r"vkCreate(?:Graphics|Compute|RayTracing)Pipelines\w*\(\s*([^,]+),\s*([^,]+),", re.S)
VULKAN_HEADERS = "include/vsg/vk/vulkan.h"


def find_vsg_root(argv: list[str]) -> pathlib.Path | None:
    if len(argv) > 1:
        return pathlib.Path(argv[1])
    env = os.environ.get("VINE_VSG_SRC_DIR")
    if env:
        return pathlib.Path(env)
    for candidate in sorted(pathlib.Path(".").glob("build*/_deps/vsg-src")):
        return candidate
    return None


def sources(root: pathlib.Path) -> list[pathlib.Path]:
    return [p for d in ("src", "include") if (root / d).is_dir() for p in (root / d).rglob("*")
            if p.suffix in (".cpp", ".h", ".hpp")]


def check_pipeline_cache(root: pathlib.Path) -> list[tuple[bool, str]]:
    results: list[tuple[bool, str]] = []

    # 1. Every pipeline-creation call in vsg's own code passes a null cache.
    callers = 0
    for path in sources(root):
        text = path.read_text(errors="replace")
        for match in PIPELINE_CALL.finditer(text):
            callers += 1
            cache_arg = match.group(2).strip()
            line = text[: match.start()].count("\n") + 1
            ok = cache_arg == "VK_NULL_HANDLE"
            results.append((ok, f"{path.relative_to(root)}:{line} passes the cache argument "
                                f"'{cache_arg}'" + ("" if ok else " -- so a cache IS injectable now")))
    if callers == 0:
        results.append((False, "no vkCreate*Pipelines call found in vsg at all -- the shape of "
                               "vsg's pipeline creation changed; re-read V3/D28"))

    # 2. Nothing else in vsg names the type (only the Vulkan headers define it).
    named: list[str] = []
    for path in sources(root):
        if path == root / VULKAN_HEADERS:
            continue
        text = path.read_text(errors="replace")
        if "VkPipelineCache" in text:
            named.append(f"{path.relative_to(root)}:{text[: text.index('VkPipelineCache')].count(chr(10)) + 1}")
    results.append((not named, "no vsg source outside the Vulkan headers names VkPipelineCache"
                    + (f" (found: {', '.join(named)})" if named else "")))

    # 3. No pipeline-cache type of vsg's own.
    own_type = [p.name for p in (root / "include").rglob("*") if "pipelinecache" in p.name.lower()]
    results.append((not own_type, "vsg ships no PipelineCache type"
                    + (f" (found: {', '.join(own_type)})" if own_type else "")))
    return results


def check_dynamic_rendering(root: pathlib.Path) -> list[tuple[bool, str]]:
    results: list[tuple[bool, str]] = []
    dynamic = [str(p.relative_to(root)) for p in (root / "src").rglob("*")
               if p.suffix in (".cpp", ".h") and "vkCmdBeginRendering" in p.read_text(errors="replace")]
    results.append((not dynamic, "vsg's recording path holds no vkCmdBeginRendering"
                    + (f" (found: {', '.join(dynamic)}) -- so a pass merge may be possible now"
                       if dynamic else "")))
    legacy = [str(p.relative_to(root)) for p in (root / "src").rglob("*")
              if p.suffix in (".cpp", ".h") and "vkCmdBeginRenderPass" in p.read_text(errors="replace")]
    results.append((bool(legacy), "vsg still records with vkCmdBeginRenderPass"
                    + (f" ({', '.join(sorted(legacy)[:2])})" if legacy else
                       " -- its recording path changed; re-read V4")))
    return results


def main(argv: list[str]) -> int:
    root = find_vsg_root(argv)
    if root is None or not root.is_dir():
        print("SKIPPED — no vsg checkout found (pass the vsg root, or set VINE_VSG_SRC_DIR)")
        return 0

    claims = [("V3/D28: vsg uses no VkPipelineCache", check_pipeline_cache(root)),
              ("V4: vsg has no dynamic-rendering recording path", check_dynamic_rendering(root))]

    findings = 0
    checked = 0
    for title, results in claims:
        print(f"{title} [{root}]")
        for ok, detail in results:
            checked += 1
            findings += 0 if ok else 1
            print(f"  {'holds ' if ok else 'CHANGED'} — {detail}")
    print(f"--- {findings} finding(s); {checked} claim(s) checked against {root}")
    if findings:
        print("A parked item's premise moved: re-read V3/V4 in .ai/memory/graphics-perf-backlog.md "
              "before assuming it is still blocked.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
