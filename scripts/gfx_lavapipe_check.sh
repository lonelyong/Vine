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
#   VINE_APP_PIXELS=0       Skip reading the app's render area (the stage otherwise
#                           requires VINE_APP_MIN_CONTENT percent of it, default 30).
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
# pipefail as well: the checks in this file judge PIPELINES (`... | head`, `... | sed`), and a pipeline's
# status is its LAST command's — so a filter that succeeded used to decide the outcome no matter what the
# command before it did. That is not hypothetical here: it is how the byte-exact evidence gate below read as
# a PASS for as long as it did (measured: `if <script> | sed ...; then` tests sed).
set -o pipefail

# ---- Locate root / build ----------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="${1:-$ROOT/build}"

VINE_BIN="$BUILD/bin/Vine"

FRAMES="${VINE_CHECK_FRAMES:-20}"
SECONDS_V="${VINE_CHECK_SECONDS:-12}"

# The app's render area is read while the app is alive (see check_app_pixels): how long to wait for the
# backend to report the window it drew into, how long to let the demo build its scene, and the share of
# pixels that must not be near-black. Measured for the default demo: the window is reported ~1.8 s after
# startup and 84.90% of the render area is non-black (378x247, lavapipe), so 30% is a margin, not a tuned
# value; the before-the-fix black render area measured 0.84%.
APP_MIN_CONTENT="${VINE_APP_MIN_CONTENT:-30}"
APP_PIXEL_WAIT="${VINE_APP_PIXEL_WAIT:-8}"
APP_PIXEL_SETTLE="${VINE_APP_PIXEL_SETTLE:-2}"

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
# Each stage counts its OWN failures here and prints its PASS line only when its own count is zero. The
# global FAILED cannot do that job: it is a boolean, so a stage in a run that was already failing cannot tell
# "one more check failed" from "nothing happened" — measured 2026-09-16, when the app stage printed [PASS]
# directly under the [FAIL] its own pixel check had just reported.
STAGE_FAILURES=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# mark_stage_failure — record that a check of the CURRENT stage failed (and that the run failed).
mark_stage_failure() {
    STAGE_FAILURES=$((STAGE_FAILURES + 1))
    FAILED=1
}

report() { # name  file
    local name="$1" file="$2"
    # Log-level gate only: whether the run DREW anything is the caller's own evidence check
    # (require_evidence), which is what keeps "no validation error" from meaning "nothing happened".
    if grep -qiE "VUID-|UNASSIGNED-|\[Validation\].*error|validation layer.*error|exception|compile failed|failed to create window|vkCreateInstance.*fail|ERROR:.*Loader" "$file"; then
        echo "[FAIL] $name"
        grep -iE "VUID-|UNASSIGNED-|\[Validation\]|exception|compile failed|failed|ERROR" "$file" | head -20
        mark_stage_failure
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
        mark_stage_failure
    fi
}

# check_app_pixels — read the LIVE app's render area and require that it is not black.
#
# The app stage judges the app by what it LOGS ("the plugin loaded", "the cube maps loaded") plus "no
# validation error" — and a run that draws nothing satisfies both. Measured 2026-09-16: a black render area
# (0.84% non-black) passed the whole gate; only reading the window's pixels found it. The self-test asserts
# pixels of its own, but the app presents to a window it cannot read back through the backend, so the pixels
# come from X: scripts/xwin2ppm.py reading the window the backend LOGGED as the one it attached to (naming
# it by title would pick the Qt container instead, whose chrome stays bright however empty the render area).
#
# VINE_APP_PIXELS=0 skips the check and VINE_APP_MIN_CONTENT (default 30, percent) is the threshold. A run
# without DISPLAY skips it too (there is no window to read); a run whose app never reports its window FAILS,
# because that line is part of the session's evidence and its absence is exactly the state this catches.
check_app_pixels() { # the caller's $app_pid and $log
    if [ "${VINE_APP_PIXELS:-1}" = "0" ]; then
        echo "    (pixels not checked, VINE_APP_PIXELS=0)"
        return
    fi
    if [ -z "${DISPLAY:-}" ]; then
        echo "    (pixels not checked: DISPLAY is unset, so there is no window to read)"
        return
    fi

    local id="" waited=0
    while [ "$waited" -lt "$APP_PIXEL_WAIT" ]; do
        id=$(sed -n 's/.*\[VsgHostWindow\] attached to the host window \(0x[0-9a-fA-F]*\).*/\1/p' "$log" | tail -1)
        [ -n "$id" ] && break
        kill -0 "$app_pid" 2>/dev/null || break
        sleep 1
        waited=$((waited + 1))
    done
    if [ -z "$id" ]; then
        echo "[FAIL] ${STAGE}: the app never reported the window it renders into, so there are no pixels to judge"
        echo "    (raise VINE_CHECK_SECONDS on a host slower than the ${APP_PIXEL_WAIT}s this waited)"
        mark_stage_failure
        return
    fi
    # The window exists before the demo's scene does: give its first frames time to build the cube maps and
    # the shadow pass, or the sample lands on a half-drawn frame.
    sleep "$APP_PIXEL_SETTLE"

    local report share
    if ! report=$(python3 "$SCRIPT_DIR/xwin2ppm.py" "$id" "$TMP/app-area.ppm" 2>&1); then
        echo "[FAIL] ${STAGE}: could not read the render area 0x${id#0x} (VINE_APP_PIXELS=0 skips this on an X setup that cannot be read)"
        printf '%s\n' "$report" | sed 's/^/    /'
        mark_stage_failure
        return
    fi
    printf '%s\n' "$report" | sed -n '1,3p' | sed 's/^/    /'
    share=$(printf '%s\n' "$report" | sed -n 's/^content (not near-black): [0-9]*\/[0-9]* = \([0-9.]*\)%$/\1/p')
    if [ -z "$share" ]; then
        echo "[FAIL] ${STAGE}: the pixel report did not say how much of the render area is not black"
        mark_stage_failure
    elif awk -v share="$share" -v want="$APP_MIN_CONTENT" 'BEGIN { exit !(share + 0 >= want + 0) }'; then
        echo "    [ok] ${share}% of the render area is not near-black (threshold ${APP_MIN_CONTENT}%)"
    else
        echo "[FAIL] ${STAGE}: the render area is ${share}% non-black, below the ${APP_MIN_CONTENT}% threshold"
        # The histogram and the PPM path are printed only here: on a failed run they ARE the evidence.
        printf '%s\n' "$report" | sed -n '4,$p' | sed 's/^/    /'
        echo "    (a session that records no frame looks exactly like this: check the attach line for"
        echo "     'mapped=' and compare its size with the window the pass was built for)"
        mark_stage_failure
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
    mark_stage_failure
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
    STAGE_FAILURES=0
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
    # The run has to have FINISHED its phases: without this, a run that hung (or was killed by the
    # timeout) after the last evidence line still had every assertion above satisfied, and the 124
    # tolerance below turned that into a PASS.
    require_evidence "^\\[selftest\\] done" 1 "self-test completion line"
    # The self-test is expected to finish (0). A timeout (124) is tolerated for a slow host, but only
    # together with the completion line above: the phases must all have run and reported.
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ]; then
        echo "[FAIL] vsg_backend_selftest exited early with $rc"
        tail -30 "$log"
        mark_stage_failure
    else
        # The self-test's pixel assertions are hard failures: a run that stayed
        # validation-clean while drawing nothing must not read as a pass.
        if grep -q "\[selftest\] FAIL" "$log"; then
            echo "[FAIL] vsg_backend_selftest reported a pixel/invariant failure"
            grep "\[selftest\] FAIL" "$log" | head -10
            mark_stage_failure
        fi
        # `report` prints the PASS line, so it only gets to speak when this stage's own checks
        # (the requires above) also passed: a missing assertion is not a pass.
        if [ "$STAGE_FAILURES" -eq 0 ]; then
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
    # The byte-exact comparison is delegated to the evidence script, which owns the baseline and PINS the
    # frame count the baseline was recorded with (the count is one of the evidence lines, so comparing a
    # 15-frame run against a 30-frame baseline could never match). The script's own exit code is what
    # decides here: judging a PIPELINE would test the filter, not the comparison (see set -o pipefail).
    evidence_log="$TMP/evidence.log"
    if "$SCRIPT_DIR/vsg_selftest_evidence.sh" "$BUILD" >"$evidence_log" 2>&1; then
        sed 's/^/    /' "$evidence_log"
        echo "[PASS] content-shading evidence matches its baseline"
    else
        sed 's/^/    /' "$evidence_log"
        echo "[FAIL] content-shading evidence differs from its baseline"
        mark_stage_failure
    fi
fi

echo "== [app] Vine app (default demo) =="
if [ "${VINE_SKIP_APP:-0}" = "1" ]; then
    echo "    (skipped, VINE_SKIP_APP=1)"
else
    if [ ! -x "$VINE_BIN" ]; then
        echo "[FAIL] Vine app not built at $VINE_BIN"
        mark_stage_failure
    else
        log="$TMP/vine.log"
        STAGE="Vine (default demo)"
        # Reset BEFORE the pixel check: the stage's own checks count here, and its PASS line below is
        # printed only while this is still zero.
        STAGE_FAILURES=0
        # Background, not foreground: the render area's pixels can only be read while the app is alive (see
        # check_app_pixels). `exec` keeps $! the app's own pid, and waiting below still yields the status the
        # foreground form did (124 when the timeout fired), so nothing else about this stage moves.
        (cd "$BUILD" && exec timeout "$SECONDS_V" ./bin/Vine) >"$log" 2>&1 &
        app_pid=$!
        check_app_pixels
        wait "$app_pid"
        rc=$?
        echo "    (exit=$rc; 124 = still running when the timeout fired, i.e. OK)"
        # The default demo has to have DONE something. The stage used to accept any run that stayed
        # validation-clean, which is exactly what it did while app_shell was failing to load: the run
        # drew no demo at all and still passed. So the demo's own evidence is required — the plugin
        # that owns the scene, the cube maps it loads from the shipped assets (the box's map AND the
        # sky's second one), and the shadow pass it asks the builder for. (The picture itself is judged by
        # check_app_pixels above, which reads the render area while the app runs; the lines below judge what
        # the app SAYS it built.)
        grep "Plugin 'app_shell' loaded" "$log" | sed 's/^/    /' || true
        grep "^\[demo\]" "$log" | sed 's/^/    /' || true
        grep "off-screen target 'shadow_map'" "$log" | sed 's/^/    /' || true
        require_evidence "Plugin 'app_shell' loaded" 1 "app_shell plugin load (without it no demo scene exists)"
        require_evidence "^\[demo\] cube map: six" 1 "cube map load from the shipped assets"
        require_evidence "sky box 'sky_box' samples it by direction" 1 "sky cube map load (the default demo's sky box)"
        require_evidence "off-screen target 'shadow_map'" 1 "shadow pass target for the demo's casting light"
        # Vine is a GUI app: it runs until killed. A timeout (124) is success;
        # any other non-zero exit indicates a startup crash.
        if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ]; then
            echo "[FAIL] Vine exited early with $rc"
            tail -30 "$log"
            mark_stage_failure
        elif [ "$STAGE_FAILURES" -eq 0 ]; then
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
