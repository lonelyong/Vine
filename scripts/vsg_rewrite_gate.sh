#!/usr/bin/env bash
# vsg_rewrite_gate.sh — the gate for the rewritten vsg backend (design §7 / §11.17 M7).
#
# WHY A SCRIPT AND NOT "THE TESTS ARE GREEN". `test_vsg` can pass while the validation layer prints
# VUIDs: nothing in gtest fails on a validation message, so the claim this backend makes - "0 VUID,
# 0 SYNC-HAZARD, and the hygiene scripts clean" - has to be checked by something that reads the
# output. That check used to be a ritual a person ran by hand (build; suite; suite again with
# VK_LAYER_ENABLES; three python scripts), which is exactly the kind of ritual that rots: the last
# stage is the one that gets skipped. This script IS the ritual, in one command, with an exit code.
#
# THE STAGES, in order (each one fails the run on its own):
#   1. build                        — `ninja -C <build>` (skip with --no-build).
#   2. device-free + device suite   — every case in `test_vsg`, INCLUDING the real-device ones. A
#                                     device case that SKIPS is not evidence: the run fails unless
#                                     VINE_GATE_ALLOW_SKIPS=1, because "every device case skipped"
#                                     would otherwise read as green.
#   3. validation layer             — the same suite with VK_LAYER_KHRONOS_validation: zero `VUID` /
#                                     `Validation Error` lines in the output.
#   4. synchronization validation   — again with VK_LAYER_ENABLES=...SYNCHRONIZATION_VALIDATION_EXT:
#                                     zero `SYNC-HAZARD` lines (skip with --quick).
#   5. hygiene                      — scripts/check_include_hygiene.py, check_diagnostic_formats.py,
#                                     check_doc_symbols.py.
#   6. the phase lines              — the `[selftest]` evidence lines the phase table prints, with the
#                                     contract that a run only ends in `[selftest] done` when every
#                                     phase passed (see core::PhaseTable).
#   7. the application              — the default demo, on a real X display: its WINDOW is read twice
#                                     (settled, and after a resize), and both pictures must pass two
#                                     criteria - the render area is not near-black, and the G-buffer
#                                     preview strip holds content - plus zero validation errors and no
#                                     warning per frame. THIS IS THE STAGE THAT CATCHES "tests green,
#                                     picture wrong": both of the rewrite's picture regressions (a
#                                     sky-only frame after a resize, squashed previews) passed every
#                                     test in this gate and were found by looking at the window. A run
#                                     without DISPLAY skips it unless VINE_GATE_ALLOW_SKIPS=1, and a run
#                                     whose demo never reports its window FAILS.
#
# A FAILING SUITE IS RUN ONCE MORE, and the retry decides the stage (the first run's failing cases are
# named in the evidence line, never hidden): one case in this suite reads a window right after a session's
# first frames, and that read can lose a race with the display server. A case that fails twice is a real
# failure. NOTE what makes that race *worse* and is therefore forbidden: an application left running by an
# earlier gate run keeps a window that overlaps the test's own, and XGetImage returns what is VISIBLE - six
# stray windows measured five to nine failures per run. The gate kills the application it starts for exactly
# this reason (see cleanup_app), and so must anyone else.
#
# Usage:
#   scripts/vsg_rewrite_gate.sh [BUILD_DIR] [--no-build] [--quick]
#
#   BUILD_DIR   build tree to use; defaults to <repo>/build.
#   --no-build  do not run ninja (gate an already-built tree).
#   --quick     skip the synchronization-validation stage (validation is still enforced).
#
# Env:
#   VK_ICD_FILENAMES       Existing selection wins; otherwise lavapipe is auto-detected
#                          (lvp_icd.json) when present, so the device cases really run.
#   VINE_GATE_ALLOW_SKIPS=1  Accept a run with skipped cases (they are printed, not hidden). Also makes
#                          a missing DISPLAY a skipped app stage instead of a failure.
#   VINE_GATE_TEST=<path>    Test binary to run instead of <BUILD_DIR>/bin/test_vsg.
#   VINE_GATE_APP=<path>     Application to run instead of <BUILD_DIR>/bin/Vine.
#   VINE_GATE_APP_RESIZE=WxH Size the demo's top-level window is dragged to (default 1120x420).
#   VINE_GATE_APP_SETTLE=N   Seconds to let the demo build and present its scene (default 4).
#   VINE_GATE_APP_AFTER=N    Seconds to settle after the resize before the second sample (default 3).
#   VINE_GATE_APP_MIN_CONTENT=P  Percent of the render area that must not be near-black (default 30).
#   VINE_GATE_APP_MIN_PREVIEW=C  Max channel the G-buffer preview strip must reach (default 64).
#   VINE_GATE_KEEP_LOGS=1    Keep the logs of a clean run too (chasing a flake across runs).
#
# Exit code 0 only when every stage is clean; 1 otherwise. The last lines are a stage summary.

set -u
# pipefail as well: the checks below judge PIPELINES (`grep -c ... | tail`), and a pipeline's status is
# its LAST command's - a filter that succeeded must not decide the outcome (the legacy gate read as a
# PASS for as long as that bug lived, see gfx_lavapipe_check.sh).
set -o pipefail

usage() {
    # EVERY comment line up to the first line that is not one - so the header cannot drift out of a hard-coded
    # line range. It did: the app stage's own docs fell outside `2,40p`, and `--help` quietly stopped describing
    # the last stage.
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "${BASH_SOURCE[0]}"
}

# ---- Arguments --------------------------------------------------------------
BUILD_DIR=""
RUN_BUILD=1
QUICK=0
for arg in "$@"; do
    case "$arg" in
        --no-build) RUN_BUILD=0 ;;
        --quick) QUICK=1 ;;
        -h | --help) usage; exit 0 ;;
        -*)
            echo "unknown option: $arg" >&2
            exit 1
            ;;
        *)
            if [ -n "$BUILD_DIR" ]; then
                echo "more than one build directory given" >&2
                exit 1
            fi
            BUILD_DIR="$arg"
            ;;
    esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -n "$BUILD_DIR" ] || BUILD_DIR="$ROOT/build"
if [ ! -d "$BUILD_DIR" ]; then
    echo "no such build directory: $BUILD_DIR (run cmake first)" >&2
    exit 1
fi
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"

TEST_BIN="${VINE_GATE_TEST:-$BUILD_DIR/bin/test_vsg}"
if [ ! -x "$TEST_BIN" ]; then
    echo "no test binary at $TEST_BIN (build it first, or pass VINE_GATE_TEST)" >&2
    exit 1
fi

# ---- Device selection --------------------------------------------------------
# A software Vulkan device is what makes the device cases run at all; without it every one of them
# SKIPS, and a skipped gate is not a gate.
if [ -z "${VK_ICD_FILENAMES:-}" ] && [ -f /usr/share/vulkan/icd.d/lvp_icd.json ]; then
    export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
fi

LOG_DIR="$(mktemp -d)"

# ONE EXIT TRAP, for two jobs: the application must not outlive the gate, and the logs are kept when
# something failed.
#
# WHY THE APPLICATION MATTERS BEYOND TIDINESS: a window that keeps rendering is what makes this suite flaky.
# XGetImage returns the VISIBLE content of a window, so a leftover window overlapping a test's own window makes
# that test read the wrong picture (measured 2026-09-24: six stray windows left five to nine cases failing per
# run, and killing them took the suite back to three runs of zero failures). The app is started with `exec` so
# the pid the gate holds IS the application's - killing a subshell around it would leave the window.
#
# VINE_GATE_KEEP_LOGS=1 keeps the logs of a clean run too (chasing a flake across runs).
APP_PID=""
cleanup_app() {
    if [ -n "$APP_PID" ] && kill -0 "$APP_PID" 2>/dev/null; then
        kill -TERM "$APP_PID" 2>/dev/null
        local waited=0
        while kill -0 "$APP_PID" 2>/dev/null && [ "$waited" -lt 20 ]; do
            sleep 0.1
            waited=$((waited + 1))
        done
        kill -KILL "$APP_PID" 2>/dev/null
    fi
    APP_PID=""
}
trap 'cleanup_app; if [ "${#FAILED_STAGES[@]}" -eq 0 ] && [ "${VINE_GATE_KEEP_LOGS:-0}" != "1" ]; then rm -rf "$LOG_DIR"; else echo "   logs kept in $LOG_DIR"; fi' EXIT

FAILED_STAGES=()
record_stage() { # name, ok(0/1), evidence
    local name="$1"
    local ok="$2"
    local evidence="$3"
    if [ "$ok" -eq 0 ]; then
        printf '  [ok]   %-28s %s\n' "$name" "$evidence"
    else
        printf '  [FAIL] %-28s %s\n' "$name" "$evidence"
        FAILED_STAGES+=("$name")
    fi
}

echo "== vsg rewrite gate =="
echo "   build:  $BUILD_DIR"
echo "   test:   $TEST_BIN"
echo "   icd:    ${VK_ICD_FILENAMES:-<none: device cases will skip>}"

# ---- 1. build ----------------------------------------------------------------
if [ "$RUN_BUILD" -eq 1 ]; then
    ninja -C "$BUILD_DIR" >"$LOG_DIR/build.log" 2>&1
    build_status=$?
    if [ "$build_status" -ne 0 ]; then
        tail -20 "$LOG_DIR/build.log"
        record_stage "build" 1 "ninja failed (see above)"
    else
        record_stage "build" 0 "ninja clean"
    fi
else
    record_stage "build" 0 "skipped (--no-build)"
fi

# ---- 2-4. the suite, with and without the validation layers -------------------
run_suite() { # logfile, extra env assignments...
    local log="$1"
    shift
    # shellcheck disable=SC2068  # the extra arguments are VAR=VALUE pairs on purpose
    env VK_INSTANCE_LAYERS="${VK_INSTANCE_LAYERS:-VK_LAYER_KHRONOS_validation}" $@ "$TEST_BIN" >"$log" 2>&1
    return $?
}

suite_cases() { grep -c '^\[       OK \]' "$1" || true; }

check_suite() { # stage-name, logfile, needs-cases(0/1)    local name="$1"
    local log="$2"
    local require_cases="$3"

    local status=0
    local failed_tests
    failed_tests="$(grep -c '^\[  FAILED  \]' "$log" || true)"
    local vuid
    vuid="$(grep -cE 'VUID|Validation Error' "$log" || true)"
    local hazards
    hazards="$(grep -c 'SYNC-HAZARD' "$log" || true)"
    local skipped
    skipped="$(grep -c '^\[  SKIPPED \]' "$log" || true)"
    local cases
    cases="$(suite_cases "$log")"

    local evidence="cases=$cases failed=$failed_tests vuid=$vuid hazard=$hazards skipped=$skipped"
    if [ -n "${4:-}" ]; then
        evidence="$evidence $4"
    fi
    if [ "$failed_tests" -ne 0 ] || [ "$vuid" -ne 0 ] || [ "$hazards" -ne 0 ]; then
        status=1
    fi
    if [ "$require_cases" -eq 1 ] && [ "$cases" -eq 0 ]; then
        status=1
        evidence="$evidence (no case reported OK: the run proved nothing)"
    fi
    if [ "$skipped" -ne 0 ] && [ "${VINE_GATE_ALLOW_SKIPS:-0}" != "1" ]; then
        status=1
        grep '^\[  SKIPPED  \]' "$log" | sed 's/^/         /'
        evidence="$evidence (set VINE_GATE_ALLOW_SKIPS=1 to accept skips)"
    fi
    if [ "$status" -ne 0 ]; then
        grep -E '^\[  FAILED  \]' "$log" | sed 's/^/         /'
        grep -nE 'VUID|Validation Error|SYNC-HAZARD' "$log" | head -5 | sed 's/^/         /'
    fi
    record_stage "$name" "$status" "$evidence"
    return "$status"
}

if [ "$QUICK" -eq 0 ]; then
    # A SUITE THAT FAILED IS RUN ONCE MORE, and the retry is what decides the stage - with the first run's
    # failing cases NAMED in the evidence, never hidden. WHY THE SECOND CHANCE: one case in this suite reads a
    # window right after a session's first frames, and in a full run the picture can still be missing from the
    # window at that instant (measured: one failing sync-validation run out of three, always the same case, and
    # the case passes on its own). A gate that goes red for a race nobody can act on is a gate that gets
    # ignored; a case that fails TWICE is a real failure, because the second run is the one that decides.
    run_suite_with_retry() { # stage-name, logfile, extra env assignments...
        local name="$1"
        local log="$2"
        shift 2
        if run_suite "$log" "$@"; then
            check_suite "$name" "$log" 1
            return
        fi
        local failed_first
        failed_first="$(grep -E '^\[  FAILED  \]' "$log" | sed 's/^\[  FAILED  \] //' | tr '\n' ' ')"
        [ -n "$failed_first" ] || failed_first="(the binary exited non-zero without a FAILED line)"
        local retry_status=0
        if ! run_suite "$log.retry" "$@"; then
            retry_status=1
        fi
        check_suite "$name" "$log.retry" 1 "(retried after: $failed_first)"
        local checked=$?
        # A non-zero exit whose log names no failing case is a CRASH, and the case table alone would call it a
        # clean suite (every case it listed was OK).
        if [ "$retry_status" -ne 0 ] && [ "$checked" -eq 0 ]; then
            record_stage "$name exit status" 1 "the test binary exited non-zero in both runs (first: $failed_first)"
        fi
    }
else
    # --quick still runs the suite; it is the synchronization-validation run that it skips (see below). The
    # helper is defined on this path too, so the rest of the script reads the same either way.
    run_suite_with_retry() {
        local name="$1"
        local log="$2"
        shift 2
        if run_suite "$log" "$@"; then
            check_suite "$name" "$log" 1
        else
            check_suite "$name" "$log" 1
            record_stage "$name exit status" 1 "the test binary exited non-zero"
        fi
    }
fi
run_suite_with_retry "suite (validation)" "$LOG_DIR/suite.log"
if [ "$QUICK" -eq 0 ]; then
    run_suite_with_retry "sync validation" "$LOG_DIR/suite_sync.log" \
        VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
else
    record_stage "sync validation" 0 "skipped (--quick)"
fi

# ---- 5. hygiene --------------------------------------------------------------
run_hygiene() { # name, script
    local name="$1"
    local script="$2"
    local out
    out="$(python3 "$ROOT/$script" 2>&1)"
    local status=$?
    local finding
    finding="$(printf '%s\n' "$out" | tail -1)"
    record_stage "$name" "$status" "$finding"
    if [ "$status" -ne 0 ]; then
        printf '%s\n' "$out" | tail -10 | sed 's/^/         /'
    fi
}
run_hygiene "include hygiene" "scripts/check_include_hygiene.py"
run_hygiene "diagnostic formats" "scripts/check_diagnostic_formats.py"
run_hygiene "doc symbols" "scripts/check_doc_symbols.py"

# ---- 6. the phase lines ------------------------------------------------------
PHASES="$(grep '^\[selftest\]' "$LOG_DIR/suite.log" || true)"
if [ -z "$PHASES" ]; then
    record_stage "phase lines" 1 "the suite printed no [selftest] line"
else
    printf '%s\n' "$PHASES" | sed 's/^/         /'
    phase_lines=$(printf '%s\n' "$PHASES" | grep -c '^\[selftest\]' || true)
    phase_failed=$(printf '%s\n' "$PHASES" | grep -c 'FAILED' || true)
    phase_closed=$(printf '%s\n' "$PHASES" | grep -c '^\[selftest\] done$' || true)
    # Several tables print (the plan-side phases and the device phases): what makes a run clean is that
    # NO phase reported FAILED and that every run that started also closed - not that the last line is a
    # `done`, which a later, passing run would also satisfy.
    if [ "$phase_failed" -ne 0 ]; then
        record_stage "phase lines" 1 "$phase_failed phase(s) reported FAILED"
    elif [ "$phase_closed" -eq 0 ]; then
        record_stage "phase lines" 1 "no phase run closed with [selftest] done"
    else
        record_stage "phase lines" 0 "$phase_lines line(s) in $phase_closed phase run(s), all closed"
    fi
fi

# ---- 7. the application ------------------------------------------------------
# WHAT THIS STAGE IS FOR. Everything above drives the plugin from a test harness. What a person actually
# looks at is the APPLICATION, and the failures it has are the ones a suite cannot see: a picture that is
# only sky (the off-screen chain drew nothing), a resize that leaves the old size on screen, a warning per
# frame. Both of those happened while `test_vsg` was green (measured 2026-09-23), which is why the gate runs
# the real application, reads its WINDOW (scripts/xwin2ppm.py) and judges TWO pictures - one settled, one
# after a size change - plus the application's own log.
#
# THE TWO PIXEL CRITERIA, and why these two:
#
#   * the render area is not near-black: a black window used to pass "no validation error" for a whole run
#     (measured 2026-09-16, which is why scripts/xwin2ppm.py exists at all);
#   * the G-buffer PREVIEW STRIP holds content (its max channel reaches the threshold). The previews are the
#     demo's full-screen copies of its off-screen attachments, so they answer "did the deferred chain draw
#     anything" - the question the first criterion cannot answer, because a dead chain still gets a
#     colourful sky over it (the window measured 84% non-black while the deferred half drew nothing).
#
# Both are asserted BEFORE and AFTER the resize, because that is where the swapchain, the off-screen chain and
# every viewport move at once. The preview geometry (four 160x90 slots at x = 8 + 168*i, y = 8, each holding
# the source's aspect inside it - see AppShellDemo's fitPreviewRect) is the DEMO's own layout: this stage reads
# the demo's evidence, so a demo that moves its previews has to move this recipe with it.
app_pixel_ids() { # the pixel report on stdin -> "<width> <height>" of the render area
    sed -n 's/^window 0x[0-9a-fA-F]*: \([0-9]*\)x\([0-9]*\).*/\1 \2/p' | tail -1
}

app_preview_rect() { # width height -> "x y w h" of the second preview's drawn rectangle
    awk -v w="$1" -v h="$2" 'BEGIN{
        slot_x = 8 + 168; slot_y = 8; slot_w = 160; slot_h = 90;
        scale = (slot_w / w < slot_h / h) ? slot_w / w : slot_h / h;
        rw = int(w * scale + 0.5); rh = int(h * scale + 0.5);
        if (rw < 1) rw = 1; if (rh < 1) rh = 1;
        print slot_x + int((slot_w - rw) / 2), slot_y + int((slot_h - rh) / 2), rw, rh
    }'
}

app_sample() { # ppm, label -> fills APP_SAMPLE_{CONTENT,PREVIEW,SIZE}; returns 1 on a failed read
    local ppm="$1"
    local label="$2"
    local report
    if ! report="$(python3 "$ROOT/scripts/xwin2ppm.py" "$APP_WINDOW" "$ppm" 2>&1)"; then
        echo "${label}: could not read the render area 0x${APP_WINDOW#0x} ($report)" >&2
        return 1
    fi
    read -r width height <<<"$(app_pixel_ids <<<"$report")"
    if [ -z "${width:-}" ]; then
        echo "${label}: the pixel report did not say the window's size ($report)" >&2
        return 1
    fi
    local rect
    rect="$(app_preview_rect "$width" "$height")"
    APP_SAMPLE_CONTENT="$(sed -n 's/^content (not near-black): [0-9]*\/[0-9]* = \([0-9.]*\)%$/\1/p' <<<"$report")"
    APP_SAMPLE_PREVIEW="$(python3 "$ROOT/scripts/ppmprobe.py" "$ppm" $rect | sed -n 's/.*max \([0-9]*\) .*/\1/p')"
    APP_SAMPLE_SIZE="${width}x${height}"
    if [ -z "$APP_SAMPLE_CONTENT" ] || [ -z "$APP_SAMPLE_PREVIEW" ]; then
        echo "${label}: the samples did not report a share and a preview maximum" >&2
        return 1
    fi
    return 0
}

app_known_vuids() { # the VUID names this stage tolerates, one per line, with the reason in the comment below
    # AN ALLOW-LIST, NOT A FILTER: every name here is still COUNTED and printed in the stage's evidence line, and
    # any VUID that is not on this list fails the stage. The two names below are the app path's own defects, and
    # they are documented with their mechanisms in .ai/design/vsg-reimplementation.md:
    #
    #   * VUID-vkCmdDraw-None-09600 - a sampled descriptor declares SHADER_READ_ONLY while its image is in
    #     UNDEFINED at the moment the command buffer is SUBMITTED. It takes a frame that reaches a sampling call
    #     before anything wrote the image: the first frames of a session and the first frame after a resize.
    #   * VUID-vkUpdateDescriptorSets-None-03047 - a descriptor set the pool handed back is written while a
    #     command buffer of an earlier frame still uses it. The screen path rebuilds its set every frame because
    #     the shadow block it binds bakes a per-frame offset into the descriptor; the content path solves the
    #     same problem with UNIFORM_BUFFER_DYNAMIC bindings (see api/BlockDescriptors).
    printf '%s\n' "VUID-vkCmdDraw-None-09600" "VUID-vkUpdateDescriptorSets-None-03047"
}

check_app() {
    local app_bin="${VINE_GATE_APP:-$BUILD_DIR/bin/Vine}"
    local settle="${VINE_GATE_APP_SETTLE:-4}"
    local after="${VINE_GATE_APP_AFTER:-3}"
    local want_content="${VINE_GATE_APP_MIN_CONTENT:-30}"
    local want_preview="${VINE_GATE_APP_MIN_PREVIEW:-64}"
    local drag="${VINE_GATE_APP_RESIZE:-1120x420}"

    if [ ! -x "$app_bin" ]; then
        record_stage "app (deferred demo)" 1 "no application at $app_bin (build it, or pass VINE_GATE_APP)"
        return
    fi
    if [ -z "${DISPLAY:-}" ]; then
        # A window needs a display: no display is a SKIP, and the same rule as a skipped case applies - a
        # gate that silently drops a stage reads as green without having judged anything.
        if [ "${VINE_GATE_ALLOW_SKIPS:-0}" = "1" ]; then
            record_stage "app (deferred demo)" 0 "skipped (DISPLAY unset; accepted by VINE_GATE_ALLOW_SKIPS=1)"
        else
            record_stage "app (deferred demo)" 1 "DISPLAY is unset, so no application window can be judged (set VINE_GATE_ALLOW_SKIPS=1 to accept)"
        fi
        return
    fi

    local log="$LOG_DIR/app.log"
    # `exec` matters: without it the job is a SUBSHELL, the gate's kill reaches the subshell, and the
    # application keeps its window and keeps drawing into it (see cleanup_app) - which is how the stray
    # windows that made this suite flaky were left behind in the first place.
    ( cd "$BUILD_DIR" && exec env QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}" VINE_VSG_DEBUG_LAYER=1 \
        "$app_bin" >"$log" 2>&1 ) &
    APP_PID=$!
    local app_pid="$APP_PID"

    # 1. The window the backend LOGS as the one it renders into (a name or title would pick the host's
    #    container, whose chrome stays bright however empty the render area is).
    local waited=0
    APP_WINDOW=""
    while [ "$waited" -lt 25 ]; do
        APP_WINDOW="$(sed -n 's/.*attached to the host window \(0x[0-9a-fA-F]*\).*/\1/p' "$log" | tail -1)"
        [ -n "$APP_WINDOW" ] && break
        kill -0 "$app_pid" 2>/dev/null || break
        sleep 1
        waited=$((waited + 1))
    done
    if [ -z "$APP_WINDOW" ]; then
        kill "$app_pid" 2>/dev/null
        wait "$app_pid" 2>/dev/null
        record_stage "app (deferred demo)" 1 "the application never reported a window (see $log)"
        return
    fi

    # 2. The settled picture, then the same picture after a resize the app has to follow. Both are measured
    #    the same way and both are judged: a picture that was already broken BEFORE the resize is a failure
    #    of this stage too, and saying which sample failed is the difference between a diagnosis and a red row.
    sleep "$settle"
    local status=0 evidence=""
    local before_content="" before_preview="" before_size=""
    local after_content="" after_preview="" after_size=""
    if app_sample "$LOG_DIR/app-before.ppm" "before the resize"; then
        before_content="$APP_SAMPLE_CONTENT" before_preview="$APP_SAMPLE_PREVIEW" before_size="$APP_SAMPLE_SIZE"
        evidence="before ${before_size}: content ${before_content}%, preview ${before_preview};"
    else
        status=1
    fi
    if [ "$status" -eq 0 ]; then
        local drag_w="${drag%x*}" drag_h="${drag#*x}"
        if ! python3 "$ROOT/scripts/xwinresize.py" "$APP_WINDOW" "$drag_w" "$drag_h" >/dev/null 2>&1; then
            echo "         (the resize request failed; the after-resize sample is skipped)" >&2
            status=1
        else
            sleep "$after"
            if app_sample "$LOG_DIR/app-after.ppm" "after the resize"; then
                after_content="$APP_SAMPLE_CONTENT" after_preview="$APP_SAMPLE_PREVIEW" after_size="$APP_SAMPLE_SIZE"
                evidence="$evidence after ${after_size}: content ${after_content}%, preview ${after_preview};"
            else
                status=1
            fi
        fi
    fi

    kill "$app_pid" 2>/dev/null
    wait "$app_pid" 2>/dev/null
    cleanup_app

    # 3. The application's own log: the layer was asked for, so a VUID is a failure, and a warning per frame
    #    is one too (a flood is how a broken episode reads; the demo's one first-frame warning is expected).
    #    A VUID this stage knows about is counted, printed, and does NOT fail it (see app_known_vuids): the gate
    #    is here to catch what nobody knows about yet. One error is one header line - the message BODY repeats
    #    the same VUID next to the spec link, and counting lines would count every error twice.
    local vuid=0 unknown=0
    local known_list
    known_list="$(app_known_vuids)"
    local count name
    while read -r count name; do
        [ -n "${name:-}" ] || continue
        vuid=$((vuid + count))
        if printf '%s\n' "$known_list" | grep -qxF "$name"; then
            evidence="$evidence $name=$count (known);"
        else
            unknown=$((unknown + count))
            evidence="$evidence $name=$count (UNKNOWN);"
        fi
    done <<<"$(grep -oE '^Validation Error: \[ VUID-[A-Za-z0-9-]+' "$log" | sed 's/.*\[ //' | sort | uniq -c || true)"
    local warnings
    warnings="$(grep -c '\[warning\]' "$log" || true)"
    evidence="$evidence vuid=$vuid warnings=$warnings"
    if [ "$unknown" -ne 0 ]; then
        grep -nE '^Validation Error' "$log" | grep -vF -f <(printf '%s\n' "$known_list") | head -5 | sed 's/^/         /'
        evidence="$evidence ($unknown VUID(s) this gate does not know about)"
        status=1
    fi
    if [ "$warnings" -gt 5 ]; then
        grep -n '\[warning\]' "$log" | head -5 | sed 's/^/         /'
        evidence="$evidence (a warning per frame is a flood, see the lines above)"
        status=1
    fi
    if [ "$status" -ne 0 ]; then
        tail -15 "$log" | sed 's/^/         /'
        echo "         (the pictures are $LOG_DIR/app-before.ppm and $LOG_DIR/app-after.ppm)" >&2
    fi
    # 4. The thresholds, per sample.
    local sample
    for sample in "before $before_size $before_content $before_preview" "after $after_size $after_content $after_preview"; do
        # Field by field rather than through `set --`: a sample that failed to read has fewer fields, and `set -u`
        # turned that into an unbound variable instead of the failure it already was.
        local when="${sample%% *}" rest="${sample#* }"
        local size="${rest%% *}" rest2="${rest#* }"
        [ "$size" != "$rest" ] || size=""
        local content="${rest2%% *}" preview="${rest2#* }"
        [ -n "$size" ] || continue
        if ! awk -v have="$content" -v want="$want_content" 'BEGIN { exit !(have + 0 >= want + 0) }'; then
            evidence="$evidence ($when: content ${content}% < ${want_content}%)"
            status=1
        fi
        if ! awk -v have="$preview" -v want="$want_preview" 'BEGIN { exit !(have + 0 >= want + 0) }'; then
            evidence="$evidence ($when: the G-buffer preview strip holds ${preview} < ${want_preview})"
            status=1
        fi
    done
    record_stage "app (deferred demo)" "$status" "$evidence"
}
check_app

# ---- Summary -----------------------------------------------------------------
echo "== summary =="
if [ "${#FAILED_STAGES[@]}" -eq 0 ]; then
    echo "   every stage clean: 0 VUID, 0 SYNC-HAZARD, hygiene clean, phases closed, app picture judged"
    exit 0
fi
echo "   failed stage(s): ${FAILED_STAGES[*]}"
exit 1
