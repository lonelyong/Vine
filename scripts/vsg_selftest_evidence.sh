#!/usr/bin/env bash
# vsg_selftest_evidence.sh — Regression check of the vsg backend self-test's EVIDENCE lines.
#
# The self-test (bin/vsg_backend_selftest) prints one `[selftest] ...` line per
# phase that passed, plus a per-phase summary line; those lines ARE the evidence
# a run is judged by, and they are checked byte-for-byte against a stored
# baseline. Everything else the run writes to stderr is backend trace
# (`[VsgRenderer] ...`, glslang output), which is deliberately NOT the criterion:
#   * the trace lines carry spdlog's `[file.cpp:line]` stamp, so any edit that
#     shifts lines in a backend file rewrites every one of them without a single
#     behaviour change;
#   * the trace is a per-frame narration ("retired (detached) ...", "EXPERIMENTAL
#     off-screen target ... attached"), and its line COUNT depends on how many
#     frames of a phase reached the device: a run whose artifacts are not
#     internally consistent (a stale plugin .so next to a freshly linked
#     libviGraphics) traces one fewer retirement while every `[selftest]` line,
#     and therefore every assertion, is identical. Comparing the full trace then
#     reports a "regression" that does not exist — the numbers below are the ones
#     that hold for a consistent build of this tree and of the commit before the
#     batch that added them (measured, see .ai/memory/graphics.md).
#
# Usage:
#   scripts/vsg_selftest_evidence.sh [BUILD_DIR]   BUILD_DIR defaults to <root>/build
#   scripts/vsg_selftest_evidence.sh --update      Rewrite the baseline from this build
#   scripts/vsg_selftest_evidence.sh --builtin     Check the BUILT-IN vsg phong path
#                                                  (runs with VINE_VSG_BUILTIN=1 against
#                                                   vsg_selftest_builtin_evidence.txt)
#
# Two baselines because the two paths legitimately draw different pictures: our
# own forward set is the SHIPPED default since P0.3, and the built-in vsg phong
# set is the opt-out fallback; they shade differently (measured, see
# .ai/design/vsg-custom-shader.md §11), so one baseline cannot cover both — and
# without the second one a change to the built-in fallback (or to the wiring that
# feeds it) would be invisible to this gate.
#
# Exit code 0 when the evidence matches the baseline, 1 otherwise.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

UPDATE=0
MODE=default
POSITIONAL=()
for arg in "$@"; do
    case "$arg" in
        --update) UPDATE=1 ;;
        --builtin) MODE=builtin ;;
        *) POSITIONAL+=("$arg") ;;
    esac
done
BUILD="${POSITIONAL[0]:-$ROOT/build}"

if [ "$MODE" = builtin ]; then
    BASELINE="$SCRIPT_DIR/vsg_selftest_builtin_evidence.txt"
    export VINE_VSG_BUILTIN=1
else
    BASELINE="$SCRIPT_DIR/vsg_selftest_evidence.txt"
fi

SELFTEST="$BUILD/bin/vsg_backend_selftest"
if [ ! -x "$SELFTEST" ]; then
    echo "vsg_selftest_evidence.sh: no self-test binary at '$SELFTEST'" >&2
    exit 1
fi

RAW="$(mktemp)"
CURRENT="$(mktemp)"
trap 'rm -f "$RAW" "$CURRENT"' EXIT

"$SELFTEST" > "$RAW" 2>&1
grep '^\[selftest\]' "$RAW" > "$CURRENT"
LINES="$(wc -l < "$CURRENT" | tr -d ' ')"
if [ "$UPDATE" = 1 ]; then
    cp "$CURRENT" "$BASELINE"
    echo "vsg_selftest_evidence.sh: baseline updated ($LINES evidence line(s), mode=$MODE)"
    exit 0
fi

if [ ! -f "$BASELINE" ]; then
    echo "vsg_selftest_evidence.sh: no baseline at '$BASELINE' (run with --update)" >&2
    exit 1
fi

if diff -q "$BASELINE" "$CURRENT" > /dev/null; then
    echo "RESULT: PASS — $LINES self-test evidence line(s) identical to the baseline (mode=$MODE)."
    exit 0
fi

echo "RESULT: FAIL — the self-test evidence does not match the baseline (mode=$MODE):" >&2
diff "$BASELINE" "$CURRENT" >&2
exit 1
