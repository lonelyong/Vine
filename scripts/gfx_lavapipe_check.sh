#!/usr/bin/env bash
# gfx_lavapipe_check.sh — Regression check of the vsg rendering backend on the
# lavapipe (Mesa software) Vulkan driver with the Khronos validation layer.
#
# This is the GPU-free end-to-end validation for vsg-backend changes. It runs:
#   1. vsg_backend_selftest — the off-screen/MRT/PiP/deferred/multi-pass phases,
#      with its `[selftest]` evidence compared byte-for-byte (vsg_selftest_evidence.sh).
#   1b. The byte-exact evidence baseline of the content-shading path (the engine's
#      own sets, exercised by 1), see .ai/design/vsg-custom-shader.md §11.
#   2. Vine app (default demo), which has to show its own evidence too: the
#      app_shell plugin loaded, the cube maps loaded from the shipped assets
#      (the box's map and the sky's second one) and the shadow pass built. A
#      run that stays validation-clean while the demo builds nothing is a FAIL
#      — that is precisely how a plugin that stopped loading once read as a
#      pass (see require_evidence below).
#
# It used to open with vsg_color_probe — a standalone vsg executable rendering a
# box through vsg's OWN Builder/phong path, to isolate the vendored vsg's
# behaviour from Vine's. The engine no longer has anything that runs through
# vsg's built-in shading (see detail::makeContentShaderSet), so a probe of that
# path validates a code path the engine cannot reach; it was deleted with it.
#
# The RenderStateMapper unit mapping (incl. the non-default StateNode path) is
# pinned by tests/test_vsg/RenderStateMapperTest, which needs no device; the
# StateNode -> distinct-pipeline path was additionally validated once on
# lavapipe with a temporary demo hook (see .ai/memory/graphics.md). Pixel-level
# visuals (culling winding, blend result, reverse-Z depth look) still require a
# real GPU.
#
# Usage:
#   scripts/gfx_lavapipe_check.sh [BUILD_DIR]     BUILD_DIR defaults to <root>/build
#
# Env overrides:
#   VK_ICD_FILENAMES        Existing selection wins; otherwise lavapipe is
#                           auto-detected (lvp_icd.json).
#   VINE_CHECK_FRAMES       Frames per probe run   (default 20)
#   VINE_CHECK_SECONDS      Seconds to run Vine    (default 12)
#   VINE_SKIP_APP=1         Skip the Vine app run.
#   VINE_VSG_DEBUG_LAYER    Existing value wins; defaults to ON when the
#                           Khronos validation layer is installed (0 disables).
#
# The self-test asserts PIXELS, not just "no validation error": it reads back
# an off-screen target (RenderBackend::readColorBuffer) and requires that a lit
# quad reached it and that its corners still hold the clear colour, so a run
# that stayed validation-clean while drawing nothing FAILS. The device the run
# exercised is printed as "[info] Vulkan device: ..." — on a machine without a
# GPU driver that is a software rasteriser (llvmpipe / lavapipe).
#
# Exit code 0 when every stage is clean, 1 otherwise.

set -u

# ---- Locate root / build ----------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="${1:-$ROOT/build}"

VINE_BIN="$BUILD/bin/Vine"

FRAMES="${VINE_CHECK_FRAMES:-20}"
SECONDS_V="${VINE_CHECK_SECONDS:-12}"

# ---- Select the lavapipe ICD -----------------------------------------------
ICD="${VK_ICD_FILENAMES:-}"
if [ -z "$ICD" ]; then
    for f in /usr/share/vulkan/icd.d/lvp_icd.json /etc/vulkan/icd.d/lvp_icd.json; do
        if [ -f "$f" ]; then
            ICD="$f"
            break
        fi
    done
fi
if [ -n "$ICD" ]; then
    export VK_ICD_FILENAMES="$ICD"
    echo "[info] Vulkan ICD: $ICD"
else
    echo "[warn] lavapipe ICD not found; relying on the default driver selection"
fi

# ---- Enable the Khronos validation layer by default --------------------------
# The whole point of this check is a validation-clean lavapipe run, so the
# layer is turned on unless the caller opted out (VINE_VSG_DEBUG_LAYER=0) or the
# layer is not installed (warn and proceed without it so the rest of the smoke
# still runs, e.g. on machines without vulkan-validationlayers). The vsg backend
# reads VINE_VSG_DEBUG_LAYER to enable validation on its window/device.
if [ -z "${VINE_VSG_DEBUG_LAYER:-}" ]; then
    VLAYER=""
    for f in /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json \
             /etc/vulkan/explicit_layer.d/VkLayer_khronos_validation.json; do
        if [ -f "$f" ]; then
            VLAYER="$f"
            break
        fi
    done
    if [ -n "$VLAYER" ]; then
        export VINE_VSG_DEBUG_LAYER=1
        echo "[info] Khronos validation layer enabled (VINE_VSG_DEBUG_LAYER=1)"
    else
        echo "[warn] Khronos validation layer not found; validation disabled"
    fi
fi

# ---- Checks -----------------------------------------------------------------
FAILED=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

report() { # name  file  [ok_exit_codes...]
    local name="$1" file="$2"
    shift 2
    if grep -qiE "VUID-|UNASSIGNED-|\[Validation\].*error|validation layer.*error|exception|compile failed|failed to create window|vkCreateInstance.*fail|ERROR:.*Loader" "$file"; then
        echo "[FAIL] $name"
        grep -iE "VUID-|UNASSIGNED-|\[Validation\]|exception|compile failed|failed|ERROR" "$file" | head -20
        FAILED=1
    elif [ $# -gt 0 ]; then
        # Expected (non-zero) exit codes passed as ok; anything else is fatal.
        grep -q "done\|running\|sync" "$file" || true
        echo "[PASS] $name"
    else
        echo "[PASS] $name"
    fi
}

# require_evidence <pattern> <minimum> <label>
#
# Counts lines matching <pattern> in the log the CURRENT stage captured (the
# caller's `$log`) and fails the run when there are fewer than <minimum>. It is
# what makes a stage's PASS mean "the thing under test reported that it ran",
# not "the process did not complain": a stage whose assertions were removed, or
# whose subject never got built, otherwise reads as green — the app stage did
# exactly that while app_shell was failing to load.
require_evidence() { # pattern minimum label
    local found
    found=$(grep -c "$1" "$log" || true)
    if [ "${found:-0}" -lt "$2" ]; then
        echo "[FAIL] ${STAGE}: ${found:-0} '$3' line(s), expected at least $2"
        FAILED=1
    fi
}

# vsg_backend_selftest: drives the real vsg RenderBackend over many frames to
# exercise the GPU paths unit tests cannot reach — off-screen MRT targets,
# PiP sampling (drawScreenTexture), deferred fullscreen programs
# (drawScreenProgram), multi-pass sharing of camera/target/scene/viewport,
# per-frame hot edits and resource-release teardown.
echo "== 1/2 vsg_backend_selftest (offscreen/MRT/PiP/deferred/multi-pass) =="
SELF="$BUILD/bin/vsg_backend_selftest"
if [ ! -x "$SELF" ]; then
    echo "[FAIL] vsg_backend_selftest not built"
    FAILED=1
else
    log="$TMP/selftest.log"
    SELF_FRAMES="${VINE_SELFTEST_FRAMES:-15}"
    (cd "$BUILD" && VINE_SELFTEST_FRAMES="$SELF_FRAMES" timeout "$SECONDS_V" ./bin/vsg_backend_selftest) >"$log" 2>&1
    rc=$?
    echo "    (exit=$rc; 124 = still running when the timeout fired, i.e. OK)"
    # Which device this run actually exercised: a green result is only
    # attributable when the driver is part of the output (a software rasteriser
    # and a real GPU do not exercise the same code paths).
    device=$(grep -m1 "\[VsgRenderer\] device:" "$log" | sed 's/^.*device: //')
    [ -n "$device" ] && echo "[info] Vulkan device: $device"
    # The pixel assertions ARE the stage: show what they measured, so a green run
    # states the picture it verified instead of only the absence of validation
    # errors, and require them to be present at all (a stage whose assertions
    # silently disappeared must not read as a pass).
    grep "^\[selftest\] pixels:" "$log" | sed 's/^/    /' || true
    grep "^\[selftest\] depth:" "$log" | sed 's/^/    /' || true
    grep "^\[selftest\] depth load:" "$log" | sed 's/^/    /' || true
    grep "^\[selftest\] shared depth pixels:" "$log" | sed 's/^/    /' || true
    grep "^\[selftest\] mixed depth:" "$log" | sed 's/^/    /' || true
    grep "^\[selftest\] depth borrow:" "$log" | sed 's/^/    /' || true
    grep "^\[selftest\] depth testonly:" "$log" | sed 's/^/    /' || true
    grep "^\[selftest\] depth share order:" "$log" | sed 's/^/    /' || true
    grep "^\[selftest\] target description:" "$log" | sed 's/^/    /' || true
    grep "^\[selftest\] MRT " "$log" | sed 's/^/    /' || true
    # Each assertion group must report itself: a stage whose assertions were
    # removed (or silently stopped running) must not read as a pass.
    stage_before=$FAILED
    STAGE="vsg_backend_selftest"
    require_evidence "^\[selftest\] pixels:" 4 "pixel assertion"
    require_evidence "^\[selftest\] program hotspot:" 1 "fullscreen program hot-edit assertion"
    require_evidence "^\[selftest\] depth:" 1 "depth assertion"
    require_evidence "^\[selftest\] depth load:" 1 "depth-LOAD assertion"
    require_evidence "^\[selftest\] shared depth pixels:" 1 "shared-depth assertion"
    require_evidence "^\[selftest\] mixed depth:" 1 "mixed-depth-policy assertion"
    require_evidence "^\[selftest\] stacked pass:" 1 "stacked-pass (non-clearing pass) assertion"
    require_evidence "^\[selftest\] promoting preserve:" 1 "depth-promoting + preserving-pass assertion"
    require_evidence "^\[selftest\] depth only:" 1 "depth-only target (far-plane clear) assertion"
    require_evidence "^\[selftest\] depth only preserve:" 1 "depth-only target (preserved depth) assertion"
    require_evidence "^\[selftest\] clear flip:" 1 "run-time clear-policy change assertion"
    require_evidence "^\[selftest\] preserved depth:" 1 "preserved-depth-not-sampled assertion"
    require_evidence "^\[selftest\] depth sample:" 1 "sampled-depth program assertion"
    # The drop report is a renderer warning, not a selftest line: section 3 of the
    # sampled-depth phase asserts the SAME-frame revoke drops the slot that bound
    # the depth (without it the frame records a stale descriptor, so this line is
    # what proves the residual window stayed closed).
    require_evidence "dropped for this frame" 1 "same-frame depth-promotion revoke assertion"
    require_evidence "^\[selftest\] color bootstrap:" 1 "colour-bootstrap (one-frame clear) assertion"
    require_evidence "^\[selftest\] depth borrow:" 1 "depth-borrow validation assertion"
    require_evidence "^\[selftest\] depth testonly:" 1 "TestOnly depth assertion"
    require_evidence "^\[selftest\] depth share order:" 1 "depth-share ordering assertion"
    require_evidence "^\[selftest\] target description:" 1 "target description rebuild assertion"
    require_evidence "^\[selftest\] policy churn:" 1 "policy-churn (no device stall) assertion"
    require_evidence "^\[selftest\] MRT " 2 "MRT report"
    # The self-test is expected to finish (0); a timeout (124) is also OK.
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ]; then
        echo "[FAIL] vsg_backend_selftest exited early with $rc"
        tail -30 "$log"
        FAILED=1
    else
        # The self-test's pixel assertions are hard failures: a run that stayed
        # validation-clean while drawing nothing must not read as a pass.
        if grep -q "\[selftest\] FAIL" "$log"; then
            echo "[FAIL] vsg_backend_selftest reported a pixel/invariant failure"
            grep "\[selftest\] FAIL" "$log" | head -10
            FAILED=1
        fi
        # `report` prints the PASS line, so it only gets to speak when this stage's own checks
        # (the requires above) also passed: a missing assertion is not a pass.
        if [ "$FAILED" -eq "$stage_before" ]; then
            report "vsg_backend_selftest" "$log"
        else
            echo "[FAIL] vsg_backend_selftest (see the checks above)"
        fi
    fi
fi

# 1b/2: the byte-exact evidence baseline of the content-shading path. The engine's own sets are the
# only sets this backend builds (vsg's built-in sets are not used at all anymore, see
# makeContentShaderSet), so there is one baseline to match: a change in the shading shows up here as
# a colour diff.
echo "== 2/2 vsg_backend_selftest (evidence: the engine's own content shading) =="
if [ ! -x "$SELF" ]; then
    echo "[FAIL] vsg_backend_selftest not built"
    FAILED=1
else
    # The byte-exact comparison is delegated to the evidence script (which owns the baseline and runs
    # the self-test with its own fixed frame count: the frame count is part of the evidence lines, so
    # comparing a 15-frame run against a 30-frame baseline could never match).
    if "$SCRIPT_DIR/vsg_selftest_evidence.sh" "$BUILD" 2>&1 | sed 's/^/    /'; then
        echo "[PASS] content-shading evidence matches its baseline"
    else
        echo "[FAIL] content-shading evidence differs from its baseline"
        FAILED=1
    fi
fi

echo "== [app] Vine app (default demo) =="
if [ "${VINE_SKIP_APP:-0}" = "1" ]; then
    echo "    (skipped, VINE_SKIP_APP=1)"
else
    if [ ! -x "$VINE_BIN" ]; then
        echo "[FAIL] Vine app not built at $VINE_BIN"
        FAILED=1
    else
        log="$TMP/vine.log"
        (cd "$BUILD" && timeout "$SECONDS_V" ./bin/Vine) >"$log" 2>&1
        rc=$?
        echo "    (exit=$rc; 124 = still running when the timeout fired, i.e. OK)"
        # The default demo has to have DONE something. The stage used to accept any run that stayed
        # validation-clean, which is exactly what it did while app_shell was failing to load: the run
        # drew no demo at all and still passed. So the demo's own evidence is required — the plugin
        # that owns the scene, the cube maps it loads from the shipped assets (the box's map AND the
        # sky's second one), and the shadow pass it asks the builder for. (The picture itself is
        # asserted by the self-test's phases, which read pixels back; the app presents to a window
        # that cannot be read.)
        grep "Plugin 'app_shell' loaded" "$log" | sed 's/^/    /' || true
        grep "^\[demo\]" "$log" | sed 's/^/    /' || true
        grep "off-screen target 'shadow_map'" "$log" | sed 's/^/    /' || true
        stage_before=$FAILED
        STAGE="Vine (default demo)"
        require_evidence "Plugin 'app_shell' loaded" 1 "app_shell plugin load (without it no demo scene exists)"
        require_evidence "^\[demo\] cube map: six" 1 "cube map load from the shipped assets"
        require_evidence "sky box 'sky_box' samples it by direction" 1 "sky cube map load (the default demo's sky box)"
        require_evidence "off-screen target 'shadow_map'" 1 "shadow pass target for the demo's casting light"
        # Vine is a GUI app: it runs until killed. A timeout (124) is success;
        # any other non-zero exit indicates a startup crash.
        if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ]; then
            echo "[FAIL] Vine exited early with $rc"
            tail -30 "$log"
            FAILED=1
        elif [ "$FAILED" -eq "$stage_before" ]; then
            report "Vine app (default demo)" "$log"
        else
            echo "[FAIL] Vine app (default demo) (see the checks above)"
        fi
    fi
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "RESULT: PASS — lavapipe validation clean (no VUID/validation errors)."
    exit 0
else
    echo "RESULT: FAIL — see messages above (logs kept until script exit)."
    exit 1
fi
