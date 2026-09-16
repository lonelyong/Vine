#!/usr/bin/env python3
"""check_vsg_window_surface_state.py — keep VsgHostWindow's surface move honest.

vsg has no "move this window to another surface" API, so
VsgHostWindow::moveToHostSurface() does it by hand: it re-points `_window`, then
drops every piece of state the base class cached FOR THE OLD SURFACE, then lets
the base class build the new surface. The device, the render pass and every
compiled pipeline survive, which is the whole point -- but only because the
stale state was actually dropped.

That drop list is the hazard: it is a list of vsg's MEMBERS, and nothing in vsg
tells us when it grows. It is easy to get wrong the other way round too -- the
first version of this check claimed `_renderPass` had been forgotten, and adding
`_renderPass.reset()` to the move SEGFAULTED: vsg's buildSwapchain() creates a
framebuffer against that pass (Window.cpp:389) and never re-creates it, so the
pass has to survive. Which members must go therefore depends on what vsg DOES
with each one: it APPENDS to `_frames`/`_indices`, ASSIGNS the depth and
multisample images, and DEREFERENCES `_renderPass`. Those three claims are
themselves checked against the vsg build in use, so the reasoning cannot rot.

So this check reads vsg's `vsg::Window` protected section and requires every
member to be classified as exactly one of:

  * DROPPED   -- must be reset by moveToHostSurface() itself (checked against the
                 body of that function, not against a second copy of the list);
  * REFRESHED -- re-derived by the base-class calls the move makes
                 (_initSurface / _initFormats / resize);
  * KEPT      -- deliberately survives the move.

A member that fits none of them is a finding: it means a vsg upgrade added state
whose fate under the move nobody decided. A member in the tables that vsg no
longer has is a finding too, so the tables cannot rot.

Usage: python3 scripts/check_vsg_window_surface_state.py [path/to/vsg-src]
       (default: build*/_deps/vsg-src, or $VINE_VSG_SRC_DIR)
Exit status: 1 on findings; 0 with a SKIPPED line when the vsg source is not
present (a fresh checkout has not configured yet, and a check that cannot see
the answer must not pretend the answer is "fine").
"""
import os
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
OUR_SOURCE = REPO_ROOT / "src/plugins/gfx_backend_vsg/src/VsgHostWindow.cpp"
OUR_FUNCTION = "VsgHostWindow::moveToHostSurface"

# vsg::Window's protected state (vsg 1.1.16, include/vsg/app/Window.h),
# partitioned by what the move must do with it. Both directions are checked
# against moveToHostSurface(): a DROPPED member must be reset by it, and a
# REFRESHED or KEPT member must not be -- resetting the wrong one breaks vsg's
# own swapchain build rather than being merely wasteful.
DROPPED = {
    "_surface": "the VkSurfaceKHR being left behind",
    "_swapchain": "its images are the old window's, and _initSwapchain wants it gone first",
    "_frames": "buildSwapchain APPENDS; uncleared it keeps the old framebuffers in the ring",
    "_indices": "buildSwapchain APPENDS alongside _frames",
    "_depthImage": "released early: buildSwapchain assigns a fresh one",
    "_depthImageView": "released early, with its image",
    "_multisampleImage": "released early: buildSwapchain assigns a fresh one when multisampling",
    "_multisampleImageView": "released early, with its image",
}
REFRESHED = {
    # Re-derived by the calls the move makes, or by the buildSwapchain() that
    # resize() runs (assigned fresh, not appended).
    "_extent2D": "resize() re-reads the host window's geometry",
    "_imageFormat": "_initFormats() re-queries the surface; a change refuses the move",
    "_depthFormat": "_initFormats() re-derives it from the (new) format",
    "_multisampleDepthImage": "buildSwapchain assigns it when multisampling needs a resolve",
    "_multisampleDepthImageView": "assigned with it",
}
KEPT = {
    # Deliberately survives: dropping these is what would break the move.
    "_renderPass": "buildSwapchain DEREFERENCES it (Window.cpp:389); a null one crashes",
    "_instance": "the instance is not per-surface",
    "_physicalDevice": "chosen once for the instance",
    "_device": "the device and its pipelines are the point of the move",
    "_traits": "the move updates nativeWindow/width/height in it",
    "_clearColor": "set from the traits",
    "_framebufferSamples": "set from the traits",
    "_availableSemaphores": "a device-level pool of acquire semaphores",
    "_availableSemaphoreIndex": "an index into that pool",
}

# What the classification above assumes vsg's buildSwapchain() does. Checked
# against the configured vsg, so an upgrade that changes any of it is a finding
# rather than a surprise at runtime.
VSG_EVIDENCE = (
    ("buildSwapchain appends to _frames", r"_frames\.push_back\("),
    ("buildSwapchain appends to _indices", r"_indices\.push_back\("),
    ("buildSwapchain builds its framebuffers against _renderPass",
     r"Framebuffer::create\(_renderPass"),
)

MEMBER = re.compile(r"^\s+[A-Za-z_][\w:<>, ]*?\b(_[A-Za-z_]\w*)\s*(?:=[^;()]*)?;\s*$")


def find_vsg_root(argv):
    """Return the vsg source root: the directory holding include/ and src/."""
    if argv and argv[0].strip():
        return pathlib.Path(argv[0])
    env = os.environ.get("VINE_VSG_SRC_DIR")
    if env:
        return pathlib.Path(env)
    for candidate in sorted(REPO_ROOT.glob("build*/_deps/vsg-src")):
        return candidate
    return None


def read_vsg_members(path):
    """Return {member: line} for the protected section of vsg::Window."""
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    start = end = None
    for index, line in enumerate(lines):
        if start is None and re.match(r"\s*class\s+VSG_DECLSPEC\s+Window\b", line):
            start = index
        elif start is not None and re.match(r"\s*protected:", line):
            end = index
            break
    if start is None or end is None:
        return None
    members = {}
    for index in range(end + 1, len(lines)):
        line = lines[index]
        if re.match(r"\s*};", line):
            break
        if "(" in line:
            continue
        match = MEMBER.match(line)
        if match:
            members[match.group(1)] = index + 1
    return members


def read_our_dropped(path):
    """Return {member: line} for the _x.reset()/_x.clear() calls in the move."""
    text = path.read_text(encoding="utf-8", errors="replace")
    head = text.find("bool " + OUR_FUNCTION)
    if head < 0:
        head = text.find(OUR_FUNCTION + "(")
    if head < 0:
        return None
    tail = text.find("VsgHostWindow::~VsgHostWindow", head)
    body = text[head : tail if tail > 0 else len(text)]
    offset = text[:head].count("\n")
    dropped = {}
    for match in re.finditer(r"^\s*(_\w+)\.(?:reset|clear)\(\)\s*;", body, re.M):
        dropped[match.group(1)] = body[: match.start()].count("\n") + offset + 2
    return dropped


def main(argv):
    root = find_vsg_root(argv)
    vsg_header = root / "include/vsg/app/Window.h" if root else None
    if vsg_header is None or not vsg_header.is_file():
        print(f"--- SKIPPED: no vsg source under {root or 'build*/_deps/vsg-src'} (configure first)")
        return 0

    members = read_vsg_members(vsg_header)
    if members is None:
        print(f"{vsg_header}: could not find the protected section of vsg::Window")
        return 1

    dropped_in_code = read_our_dropped(OUR_SOURCE)
    if dropped_in_code is None:
        print(f"{OUR_SOURCE}: could not find {OUR_FUNCTION}()")
        return 1

    findings = 0
    classified = dict(DROPPED)
    classified.update(REFRESHED)
    classified.update(KEPT)

    # Every member of vsg::Window must have a decided fate.
    for member, line in sorted(members.items()):
        if member not in classified:
            findings += 1
            print(f"{vsg_header}:{line}: '{member}' is not classified: decide whether the "
                  f"surface move must drop, refresh or keep it")
        elif member in DROPPED and member not in dropped_in_code:
            findings += 1
            print(f"{OUR_SOURCE}:{line_of_drop_start(dropped_in_code)}: moveToHostSurface() does "
                  f"not reset '{member}' ({DROPPED[member]})")

    # ... and the move must not reset what vsg needs to survive. Resetting a KEPT
    # member is how the first version of this check broke the move.
    for member, line in sorted(dropped_in_code.items()):
        if member in DROPPED:
            continue
        findings += 1
        if member in REFRESHED:
            where, why = "refreshed by vsg", REFRESHED[member]
        elif member in KEPT:
            where, why = "kept on purpose", KEPT[member]
        else:
            where, why = "not classified by this check", ""
        print(f"{OUR_SOURCE}:{line}: moveToHostSurface() resets '{member}', but it is {where}"
              + (f" ({why})" if why else "") + ": dropping it is not the move's business")

    for member in sorted(set(classified) - set(members)):
        findings += 1
        print(f"-- '{member}' is classified here but vsg::Window no longer has it: remove the entry")

    # The classification rests on what vsg's buildSwapchain() does; check that too.
    vsg_impl = root / "src/vsg/app/Window.cpp"
    if vsg_impl.is_file():
        text = vsg_impl.read_text(encoding="utf-8", errors="replace")
        for description, pattern in VSG_EVIDENCE:
            if not re.search(pattern, text):
                findings += 1
                print(f"{vsg_impl}: vsg no longer matches '{description}': re-read the "
                      f"DROPPED / REFRESHED / KEPT tables above")

    print(f"--- {findings} finding(s); {len(members)} vsg::Window member(s), "
          f"{len(DROPPED)} dropped by the move, {len(REFRESHED)} refreshed, {len(KEPT)} kept, "
          f"{len(VSG_EVIDENCE)} claim(s) about vsg checked")
    return 1 if findings else 0


def line_of_drop_start(dropped):
    return min(dropped.values()) if dropped else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
