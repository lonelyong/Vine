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
#   VINE_GATE_ALLOW_SKIPS=1  Accept a run with skipped cases (they are printed, not hidden).
#   VINE_GATE_TEST=<path>    Test binary to run instead of <BUILD_DIR>/bin/test_vsg.
#
# Exit code 0 only when every stage is clean; 1 otherwise. The last lines are a stage summary.

set -u
# pipefail as well: the checks below judge PIPELINES (`grep -c ... | tail`), and a pipeline's status is
# its LAST command's - a filter that succeeded must not decide the outcome (the legacy gate read as a
# PASS for as long as that bug lived, see gfx_lavapipe_check.sh).
set -o pipefail

usage() {
    sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
trap 'rm -rf "$LOG_DIR"' EXIT

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

check_suite() { # stage-name, logfile, needs-cases(0/1)
    local name="$1"
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

if run_suite "$LOG_DIR/suite.log"; then
    check_suite "suite (validation)" "$LOG_DIR/suite.log" 1
else
    check_suite "suite (validation)" "$LOG_DIR/suite.log" 1
    record_stage "suite exit status" 1 "the test binary exited non-zero"
fi

if [ "$QUICK" -eq 1 ]; then
    record_stage "sync validation" 0 "skipped (--quick)"
else
    if run_suite "$LOG_DIR/suite_sync.log" \
        VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT; then
        check_suite "sync validation" "$LOG_DIR/suite_sync.log" 1
    else
        check_suite "sync validation" "$LOG_DIR/suite_sync.log" 1
        record_stage "sync exit status" 1 "the test binary exited non-zero"
    fi
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

# ---- Summary -----------------------------------------------------------------
echo "== summary =="
if [ "${#FAILED_STAGES[@]}" -eq 0 ]; then
    echo "   every stage clean: 0 VUID, 0 SYNC-HAZARD, hygiene clean, phases closed"
    exit 0
fi
echo "   failed stage(s): ${FAILED_STAGES[*]}"
exit 1
