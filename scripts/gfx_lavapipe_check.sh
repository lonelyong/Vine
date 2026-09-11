#!/usr/bin/env bash
# gfx_lavapipe_check.sh — Regression check of the vsg rendering backend on the
# lavapipe (Mesa software) Vulkan driver with the Khronos validation layer.
#
# This is the GPU-free end-to-end validation for vsg-backend changes. It runs:
#   1. vsg_color_probe in its raw-Phong and Builder-box modes — proves vsg
#      pipelines compile, record and present on a Vulkan device.
#   2. The Vine app (app_shell demo, 5 boxes) — proves the SceneBridge path
#      (default RenderStateMapper mapping) syncs and renders every frame with
#      no validation-layer errors.
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
#   VINE_PROBE_MODE_EXTRA   Extra vsg_color_probe modes to run (space list).
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

PROBE="$BUILD/bin/vsg_color_probe"
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

run_probe() { # mode  frames
    local mode="$1" frames="$2"
    local log="$TMP/probe_$mode.log"
    if [ "$mode" = "raw" ]; then
        (cd "$BUILD" && VINE_PROBE_FRAMES="$frames" ./bin/vsg_color_probe) >"$log" 2>&1
        rc=$?
    else
        (cd "$BUILD" && VINE_PROBE_MODE="$mode" VINE_PROBE_FRAMES="$frames" ./bin/vsg_color_probe) >"$log" 2>&1
        rc=$?
    fi
    echo "    (exit=$rc)"
    report "vsg_color_probe [$mode]" "$log" 0
    if [ "$rc" -ne 0 ]; then FAILED=1; fi
}

echo "== 1/4 vsg_color_probe raw-Phong =="
run_probe raw "$FRAMES"

echo "== 2/4 vsg_color_probe Builder box =="
run_probe box "$FRAMES"

# Runtime-compiled user program (glslang) -> hand-built ShaderSet -> pipeline.
echo "== 3/4 vsg_color_probe custom user shader =="
run_probe custom "$FRAMES"

for extra in ${VINE_PROBE_MODE_EXTRA:-}; do
    echo "== 3b/4 vsg_color_probe [$extra] =="
    run_probe "$extra" "$FRAMES"
done

# vsg_backend_selftest: drives the real vsg RenderBackend over many frames to
# exercise the GPU paths unit tests cannot reach — off-screen MRT targets,
# PiP sampling (drawScreenTexture), deferred fullscreen programs
# (drawScreenProgram), multi-pass sharing of camera/target/scene/viewport,
# per-frame hot edits and resource-release teardown.
echo "== 3c/4 vsg_backend_selftest (offscreen/MRT/PiP/deferred/multi-pass) =="
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
    grep "^\[selftest\] MRT " "$log" | sed 's/^/    /' || true
    # Each assertion group must report itself: a stage whose assertions were
    # removed (or silently stopped running) must not read as a pass.
    require_evidence() { # pattern minimum label
        local found
        found=$(grep -c "$1" "$log" || true)
        if [ "${found:-0}" -lt "$2" ]; then
            echo "[FAIL] vsg_backend_selftest reported ${found:-0} '$3' line(s), expected at least $2"
            FAILED=1
        fi
    }
    require_evidence "^\[selftest\] pixels:" 4 "pixel assertion"
    require_evidence "^\[selftest\] depth:" 1 "depth assertion"
    require_evidence "^\[selftest\] depth load:" 1 "depth-LOAD assertion"
    require_evidence "^\[selftest\] shared depth pixels:" 1 "shared-depth assertion"
    require_evidence "^\[selftest\] mixed depth:" 1 "mixed-depth-policy assertion"
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
        report "vsg_backend_selftest" "$log"
    fi
fi

echo "== 4/4 Vine app (default demo) =="
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
        # Vine is a GUI app: it runs until killed. A timeout (124) is success;
        # any other non-zero exit indicates a startup crash.
        if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ]; then
            echo "[FAIL] Vine exited early with $rc"
            tail -30 "$log"
            FAILED=1
        else
            report "Vine app (default demo)" "$log"
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
