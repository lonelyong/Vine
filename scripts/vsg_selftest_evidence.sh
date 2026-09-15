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
#
# Env:
#   VINE_EVIDENCE_FRAMES   Frames the baseline was recorded with (default 30). The run is PINNED to this
#                          value, so a caller that shrinks VINE_SELFTEST_FRAMES for speed cannot turn the
#                          comparison into a false difference; set it here and pass --update to rebase.
#   VINE_EVIDENCE_TIMEOUT  Seconds the run may take (default 120); a run that does not finish fails.
#
# One baseline, because there is one content-shading path: the engine's own sets.
# vsg's built-in shader sets are not used at all any more (see
# makeContentShaderSet), so the second baseline this script used to keep — the
# built-in phong fallback, run with VINE_VSG_BUILTIN=1 — no longer describes
# anything a build can produce.
#
# Exit code 0 when the evidence matches the baseline, 1 otherwise.

set -u
# Judge pipelines by every stage, not by their last one (a `... | head` that found nothing must not be
# able to speak for the command it filtered).
set -o pipefail

# The frame count the baseline was recorded with, PINNED here and exported into the run: several evidence
# lines state how many frames a phase drove ("30 frames", "policy churn: 30 frame(s)"), so inheriting the
# caller's VINE_SELFTEST_FRAMES (e.g. the 15 a faster pre-commit run uses) compares a 15-frame run against
# a 30-frame baseline and reports a difference that cannot exist. The override exists for updating the
# baseline deliberately: VINE_EVIDENCE_FRAMES=... scripts/vsg_selftest_evidence.sh --update.
FRAMES="${VINE_EVIDENCE_FRAMES:-30}"
# A self-test that never finishes must fail the comparison instead of leaving the baseline unmatched
# forever (the phases print as they go, so a hung run would otherwise have "some" evidence to diff).
TIMEOUT_S="${VINE_EVIDENCE_TIMEOUT:-120}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

UPDATE=0
POSITIONAL=()
for arg in "$@"; do
    case "$arg" in
        --update) UPDATE=1 ;;
        *) POSITIONAL+=("$arg") ;;
    esac
done
BUILD="${POSITIONAL[0]:-$ROOT/build}"

BASELINE="$SCRIPT_DIR/vsg_selftest_evidence.txt"

SELFTEST="$BUILD/bin/vsg_backend_selftest"
if [ ! -x "$SELFTEST" ]; then
    echo "vsg_selftest_evidence.sh: no self-test binary at '$SELFTEST'" >&2
    exit 1
fi

RAW="$(mktemp)"
CURRENT="$(mktemp)"
trap 'rm -f "$RAW" "$CURRENT"' EXIT

VINE_SELFTEST_FRAMES="$FRAMES" timeout "$TIMEOUT_S" "$SELFTEST" > "$RAW" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
    # Reported as a failure of the RUN, not as a difference from the baseline: "the self-test did not
    # succeed" is a different thing from "the picture changed", and a reader acting on the second would go
    # looking in the renderer. The two cases are named apart: the timeout killed it (124), or its own
    # assertions did (anything else).
    if [ "$rc" -eq 124 ]; then
        echo "vsg_selftest_evidence.sh: the self-test was still running after ${TIMEOUT_S}s, so its evidence cannot be judged:" >&2
    else
        echo "vsg_selftest_evidence.sh: the self-test failed (exit $rc), so its evidence cannot be judged:" >&2
    fi
    tail -20 "$RAW" >&2
    exit 1
fi
grep '^\[selftest\]' "$RAW" > "$CURRENT"
# NOT named LINES: bash's LINES/COLUMNS are the terminal height/width, and it re-reads them after every
# external command — so a count kept in LINES is silently replaced by the terminal's row count (measured:
# this reported "30 evidence line(s)" for a 53-line file in a 30-row terminal, in the very message a reader
# uses to judge what the gate covered).
EVIDENCE_LINES="$(wc -l < "$CURRENT" | tr -d ' ')"
if [ "$UPDATE" = 1 ]; then
    cp "$CURRENT" "$BASELINE"
    echo "vsg_selftest_evidence.sh: baseline updated ($EVIDENCE_LINES evidence line(s))"
    exit 0
fi

if [ ! -f "$BASELINE" ]; then
    echo "vsg_selftest_evidence.sh: no baseline at '$BASELINE' (run with --update)" >&2
    exit 1
fi

if diff -q "$BASELINE" "$CURRENT" > /dev/null; then
    echo "RESULT: PASS — $EVIDENCE_LINES self-test evidence line(s) identical to the baseline."
    exit 0
fi

echo "RESULT: FAIL — the self-test evidence does not match the baseline:" >&2
diff "$BASELINE" "$CURRENT" >&2
exit 1
