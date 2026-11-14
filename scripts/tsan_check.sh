#!/usr/bin/env bash
# tsan_check.sh — ThreadSanitizer gate for the graphics module.
#
# The two-worlds design (`.ai/design/graphics-threading.md`) puts a frame's
# content half on the caller's thread and its engine half on another one, and the
# engine now runs that way BY DEFAULT in a hosted window. "No two threads touch
# the same state" is a claim no amount of code reading settles, and the release
# build cannot see it either: this script builds the graphics library and its
# tests with -fsanitize=thread and runs them.
#
# What it proves, and what it found the first time it ran:
#   1. The engine thread, the delta channel, the per-view memo and the mirror
#      list share no unprotected state (0 race reports over the whole suite).
#   2. It also checks LIVENESS, which is how it paid for itself: `removePass`
#      dereferenced the pass after erasing the slot that held its last reference
#      (a use-after-free no inline test had noticed), a test read a camera it had
#      released, and two loops range-for'd over `*collectRenderCommands(...)` -
#      a temporary shared_ptr that dies at the end of the range initialisation,
#      not at the end of the loop.
#
# Usage:
#   scripts/tsan_check.sh                 # configure (once), build, run
#   VN_TSAN_FILTER='RenderEngineTest.*' scripts/tsan_check.sh
#   VN_TSAN_REBUILD=1 scripts/tsan_check.sh   # reconfigure an existing tree
#   VN_TSAN_TARGET=test_vk scripts/tsan_check.sh   # the vk backend's own suites
#
# The tree is <root>/build-tsan. It is NOT the developer's build: a sanitizer
# run is a separate artifact, and mixing the two would put instrumented and
# uninstrumented objects in one link.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build-tsan}"
FILTER="${VN_TSAN_FILTER:-*}"
# The suite is the caller's: the engine's two worlds live in test_graphics, while
# the vk backend's one-frame-at-a-time contract is exercised by test_vk.
TARGET="${VN_TSAN_TARGET:-test_graphics}"

if [[ ! -f "$BUILD/CMakeCache.txt" || "${VN_TSAN_REBUILD:-0}" == "1" ]]; then
    echo "--- configuring $BUILD (thread sanitizer)"
    cmake -S "$ROOT" -B "$BUILD" -G Ninja \
          -DCMAKE_BUILD_TYPE=Debug \
          -DCMAKE_C_COMPILER=clang-23 -DCMAKE_CXX_COMPILER=clang++-23 \
          -DCMAKE_C_FLAGS="-fsanitize=thread" \
          -DCMAKE_CXX_FLAGS="-fsanitize=thread" \
          -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
          -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread" >/dev/null
fi

echo "--- building $TARGET (thread sanitizer)"
cmake --build "$BUILD" --target "$TARGET"

LOG="$(mktemp)"
echo "--- running $TARGET (filter: $FILTER)"
# THE SUPPRESSIONS ARE THE THIRD-PARTY RULE (see tsan.supp): the Vulkan ICD and the validation layer are uninstrumented
# binaries whose own threads and locks the runtime's interceptors still see. A race that mentions our code still fails.
SUPP="$ROOT/scripts/tsan.supp"
TSAN_OPTS="halt_on_error=0 second_deadlock_stack=1 print_suppressions=1"
if [[ -f "$SUPP" ]]; then
    TSAN_OPTS="suppressions=$SUPP:$TSAN_OPTS"
fi
set +e
TSAN_OPTIONS="$TSAN_OPTS" \
    "$BUILD/bin/$TARGET" --gtest_filter="$FILTER" 2>&1 | tee "$LOG"
STATUS=${PIPESTATUS[0]}
set -e

WARNINGS=$(grep -c "WARNING: ThreadSanitizer" "$LOG" || true)
echo "--- $WARNINGS ThreadSanitizer warning(s)"
# What the suppressions absorbed is PRINTED, never hidden: a run that silently excused everything would be the same
# green as a clean one (the same rule the ASan gate follows for its leak table).
if grep -q "Suppressions used" "$LOG"; then
    sed -n '/Suppressions used/,/^$/p' "$LOG" | sed 's/^/    /'
fi

# A warning is a verdict of its own: the suite can pass while a race is real.
if [[ "$STATUS" -ne 0 || "$WARNINGS" -ne 0 ]]; then
    echo "--- FAILED (exit $STATUS, $WARNINGS warning(s)); log kept at $LOG"
    exit 1
fi
rm -f "$LOG"
echo "--- OK"
