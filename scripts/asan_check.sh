#!/usr/bin/env bash
# asan_check.sh — AddressSanitizer gate for the appfw framework code.
#
# The EventBus review left one class of claims unprovable by inspection: "this
# delivery cannot outlive that object" / "nothing touches the bus after it is
# destroyed". Those are UAF-class statements, and only an instrumented build can
# settle them. This script builds the framework and its tests in a separate
# directory (-fsanitize=address) and runs them, so the guarantee is re-checked on
# every change instead of being taken on trust.
#
# What it proves:
#   1. A queued (Main) delivery that outlives the bus, its channel and its
#      subscription never touches freed memory (the task only holds the detached
#      DeliveryRegistry and its own Payload).
#   2. Subscriptions and events are released where the design says they are
#      (shutdown, bus destruction, reaping) and are not used afterwards.
#   3. Handler closures reaped during publish/subscribe are destroyed outside
#      every lock, so their destructors cannot deadlock or corrupt state.
#
# Usage:
#   scripts/asan_check.sh [BUILD_DIR]     BUILD_DIR defaults to <root>/build-asan
#
# Wider runs:
#   VINE_ASAN_FILTER='*' scripts/asan_check.sh              # whole test_gui suite
#   VINE_ASAN_TARGET=test_core VINE_ASAN_FILTER='*' scripts/asan_check.sh
#   VINE_ASAN_LEAKS=1 scripts/asan_check.sh                 # + LeakSanitizer
#
# The vsg BACKEND, under STRICT leak judging. What a report may still contain,
# and why, is part of the judge (measured 2026-09-25, design §11.16df):
#   * the plugin loader's retention is deliberate (it never dlclose()s plugin
#     code) and is excused in asan_leaks.supp with its reason next to the
#     release() that causes it;
#   * INDIRECT LeakSanitizer entries are the same deliberate retention one hop
#     down from an excused root: the verdict passes them and PRINTS their counts
#     (a DIRECT leak still fails - see the rule below). Measured 2026-09-26:
#     ~6 KB in ~106 chunks beyond the loader's 808 B, every entry indirect, the
#     allocation frames inside glslang's SPIR-V builder whenever the symbolizer
#     resolves the plugin (see the spv::Builder entry in asan_leaks.supp).
#     NOT a claimed-clean configuration: test_gui with VINE_ASAN_FILTER='*'
#     VINE_ASAN_LEAKS=1 still reports that suite's own direct leaks (13 roots in
#     GuiTest::SetUp / buildDock, 2026-09-26) - its leak story is its own item.
#   VINE_ASAN_TARGET=test_vsg VINE_ASAN_FILTER='*' VINE_ASAN_LEAKS=1 scripts/asan_check.sh
#       (the device cases connect to X and close it again themselves - see
#        TestXConnection in tests/test_vsg/TestHostWindow.hpp - and SKIP without
#        DISPLAY. CoreAllocationGateTest's mallinfo2-teeth case skips under ASan:
#        ASan's allocator is invisible to mallinfo2.)
#   vsg_backend_selftest no longer exists: it drove the retired renderer and went
#   with it (see src/plugins/gfx_backend_vsg/CMakeLists.txt). The app gate in
#   scripts/vsg_rewrite_gate.sh is the end-to-end evidence now.
# VINE_ASAN_LEAK_SCOPE remains the FALLBACK for a run whose leaks are not excused
# yet: it reports leaks outside the pattern without failing the run. Prefer
# excusing them properly (a justified entry) over scoping them away, because a
# scope also hides the next leak in the same run.
# What the suite covers: the caches, the pools, the retire rings, the compile leases, the session
# teardown of the GPU-free paths, and - with DISPLAY + an ICD - the device phases (DevicePhaseTest):
# session build / move / shutdown and the targets it materialises, for real.
#
# COMPILER FAMILY OF THE TREE. A suite that links a C dependency (test_iobase,
# and anything reaching IOBase through loaders/robotics) needs the tree's C and
# C++ compilers from the SAME family. A tree configured with CC=gcc + CXX=clang
# mixes gcc's shared libasan into a binary that also carries clang's static
# runtime, and the run dies before any test executes with "Your application is
# linked against incompatible ASan runtimes" (measured 2026-09-25 on a tree that
# predated the CC derivation below; test_gui/test_vsg never noticed, because
# they link no C-built library). VINE_ASAN_RECONFIG=1 does NOT switch an
# existing cache's compiler - remove the build dir and let the script configure
# a consistent one.
#
# LeakSanitizer suppressions live in scripts/asan_leaks.supp and only cover
# library-internal retention (fontconfig); anything the framework leaks is still
# reported. The whole-suite leak mode additionally reports `GuiTest::buildDock`
# (tests/test_gui/test_gui.cpp), i.e. the fixture allocating dock panels that
# nobody owns - a test-fixture/DockPanel-ownership item, not an EventBus one.
#
# Env overrides:
#   VINE_ASAN_FILTER   gtest filter        (default 'EventBusTest.*')
#   VINE_ASAN_TARGET   cmake target        (default test_gui)
#   VINE_ASAN_LEAKS    1 to enable leak detection (default 0: the framework
#                      deliberately leaks QApplication/QCoreApplication, so leak
#                      reports are noise until that is fixed - see
#                      .ai/design/appfw-eventbus.md)
#   VINE_ASAN_JOBS     build parallelism   (default: nproc)
#   VINE_ASAN_RECONFIG 1 to force a re-configure of BUILD_DIR
#   VINE_ASAN_LOG      path of the run log (default: BUILD/asan_<target>.log)
#   VINE_ASAN_QPA_PLATFORM  Qt platform plugin for the run (default: offscreen).
#                      The gate must NOT inherit the desktop's plugin: under ASan
#                      the xcb plugin loads libxkbcommon-x11, which reports a read
#                      past the end of the XKB keymap in strndup and aborts the
#                      run before any test executes. Set to xcb to run against a
#                      real display anyway.
#
# Exit code 0 when the run is ASan-clean, 1 otherwise.

set -u

# ---- Locate root / build ----------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="${1:-$ROOT/build-asan}"

TARGET="${VINE_ASAN_TARGET:-test_gui}"
FILTER="${VINE_ASAN_FILTER:-EventBusTest.*}"
LEAKS="${VINE_ASAN_LEAKS:-0}"
# When set (a grep -E pattern, e.g. 'vn::vsg'), a LeakSanitizer report that mentions NO frame matching it
# is reported but does not fail the run: the run judges the pattern's own allocations. Unset = every leak
# fails, which is what the appfw gate wants. Use it only while a report is not excused yet (see the recipes
# in this file's header): a justified entry in asan_leaks.supp is the stronger answer.
LEAK_SCOPE="${VINE_ASAN_LEAK_SCOPE:-}"
JOBS="${VINE_ASAN_JOBS:-$(nproc 2>/dev/null || echo 8)}"
RECONFIG="${VINE_ASAN_RECONFIG:-0}"

CXX_BIN="${CXX:-}"
CC_BIN="${CC:-}"
if [ -z "$CXX_BIN" ]; then
    # Prefer the compiler the normal build uses, then any versioned clang/gcc.
    cached_cxx="$(sed -n 's/^CMAKE_CXX_COMPILER:FILEPATH=//p' "$ROOT/build/CMakeCache.txt" 2>/dev/null)"
    if [ -n "$cached_cxx" ] && [ -x "$cached_cxx" ]; then
        CXX_BIN="$cached_cxx"
    fi
fi
if [ -z "$CXX_BIN" ]; then
    for c in clang++ clang++-22 clang++-21 clang++-20 clang++-19 clang++-18 g++-14 g++-13 g++; do
        if command -v "$c" >/dev/null 2>&1; then
            CXX_BIN="$c"
            break
        fi
    done
fi
if [ -z "$CC_BIN" ] && [ -n "$CXX_BIN" ]; then
    case "$CXX_BIN" in
        # clang++-22 -> clang-22, /usr/bin/clang++ -> /usr/bin/clang. The delimiter is '#', not '|': the
        # pattern contains '|' itself, and sed read that as the end of the substitution (it printed
        # "unknown option to `s'" and the derivation silently never happened -- measured 2026-09-16).
        *clang*) CC_BIN="$(printf '%s' "$CXX_BIN" | sed -E 's#(^|/)clang\+\+#\1clang#')" ;;
        # g++-13 -> gcc-13, /usr/bin/g++ -> /usr/bin/gcc
        *g++*)   CC_BIN="$(printf '%s' "$CXX_BIN" | sed -E 's#(^|/)g\+\+#\1gcc#')" ;;
    esac
    if [ -n "$CC_BIN" ] && ! command -v "$CC_BIN" >/dev/null 2>&1; then
        CC_BIN=""
    fi
fi
if [ -z "$CC_BIN" ]; then
    # No C driver next to the C++ one: pick one from PATH. Pointing
    # CMAKE_C_COMPILER at a C++ compiler is not tolerated (CMake >= 4 fails the
    # compiler test with "The CMAKE_C_COMPILER is set to a C++ compiler").
    for c in gcc cc clang; do
        if command -v "$c" >/dev/null 2>&1; then
            CC_BIN="$c"
            break
        fi
    done
fi
if [ -z "$CXX_BIN" ] || ! command -v "$CXX_BIN" >/dev/null 2>&1; then
    echo "[error] no compiler with AddressSanitizer found; set CC/CXX explicitly" >&2
    exit 1
fi
if [ -z "$CC_BIN" ]; then
    echo "[error] no C compiler found next to $CXX_BIN; set CC explicitly" >&2
    exit 1
fi

# ONE ASan RUNTIME FOR THE WHOLE PROCESS (-shared-libasan). The tree runs a binary that dlopens an
# instrumented plugin; with clang's default (a STATIC runtime per DSO) the plugin carried its own copy and
# everything its code allocated was booked by that copy, so the LeakSanitizer report showed plugin-side
# allocations with garbled symbols (measured 2026-09-25: ~6 KB in ~108 small allocations, frames like
# `_Rb_tree::end()` over `<unknown module>`) and nothing in it could be attributed. With one shared runtime
# both sides book into one ledger, which is what makes the leak verdict readable. clang adds no rpath for
# it, so the linker flags carry `-print-runtime-dir`.
SAN_FLAGS="-fsanitize=address -shared-libasan -fno-omit-frame-pointer -g"
SAN_LINK_FLAGS="-fsanitize=address -shared-libasan -Wl,-rpath,$("$CXX_BIN" -print-runtime-dir)"

# ---- Configure --------------------------------------------------------------
if [ "$RECONFIG" = "1" ] || [ ! -f "$BUILD/CMakeCache.txt" ]; then
    echo "[info] configuring $BUILD (ASan, $($CXX_BIN --version | head -1))"
    cmake -S "$ROOT" -B "$BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER="$CC_BIN" \
        -DCMAKE_CXX_COMPILER="$CXX_BIN" \
        -DCMAKE_C_FLAGS="$SAN_FLAGS" \
        -DCMAKE_CXX_FLAGS="$SAN_FLAGS" \
        -DCMAKE_EXE_LINKER_FLAGS="$SAN_LINK_FLAGS" \
        -DCMAKE_SHARED_LINKER_FLAGS="$SAN_LINK_FLAGS" \
        -DCMAKE_MODULE_LINKER_FLAGS="$SAN_LINK_FLAGS" || exit 1
else
    echo "[info] reusing existing ASan build dir $BUILD"
fi

# ---- Build ------------------------------------------------------------------
# The plugin libraries are loaded by test_gui/test_vsg from this build dir, so they
# are built here as well: a stale .so is read with the current PluginInfo layout,
# which shows up as an ASan global-buffer-overflow inside the plugin's own static
# metadata and looks like a bug in the host.
PLUGIN_TARGETS="app_shell test_plugin"
if [ "$TARGET" = "test_vsg" ]; then
    PLUGIN_TARGETS="$PLUGIN_TARGETS gfx_backend_vsg"
fi

echo "[info] building targets: $TARGET $PLUGIN_TARGETS (-j$JOBS)"
cmake --build "$BUILD" --target "$TARGET" $PLUGIN_TARGETS -j "$JOBS" || exit 1

BIN="$BUILD/bin/$TARGET"
if [ ! -x "$BIN" ]; then
    echo "[error] $BIN is missing" >&2
    exit 1
fi

# A gate that silently runs an uninstrumented binary is worse than no gate. This tree links the SHARED
# runtime, so ldd names it - the nm check stays first because it also catches an instrumented binary whose
# runtime is not resolvable yet.
asan_linked=0
if command -v nm >/dev/null 2>&1 && nm "$BIN" 2>/dev/null | grep -q "__asan_init"; then
    asan_linked=1
elif ldd "$BIN" 2>/dev/null | grep -q "libclang_rt.asan\|libasan"; then
    asan_linked=1
elif command -v strings >/dev/null 2>&1 && strings "$BIN" | grep -q "AddressSanitizer"; then
    asan_linked=1
fi
if [ "$asan_linked" != "1" ]; then
    echo "[error] $BIN is not linked against the AddressSanitizer runtime" >&2
    exit 1
fi
echo "[info] $BIN is ASan-instrumented"

# ---- Run --------------------------------------------------------------------
export QT_QPA_PLATFORM="${VINE_ASAN_QPA_PLATFORM:-offscreen}"
export ASAN_OPTIONS="detect_leaks=${LEAKS}:halt_on_error=1:detect_stack_use_after_return=1:strict_string_checks=1:allocator_may_return_null=0"
export LSAN_OPTIONS="report_objects=1"
SUPP="$SCRIPT_DIR/asan_leaks.supp"
if [ -f "$SUPP" ]; then
    # Library-internal retention (e.g. fontconfig) is suppressed there; anything
    # the framework leaks still shows up.
    LSAN_OPTIONS="suppressions=$SUPP:$LSAN_OPTIONS"
    export LSAN_OPTIONS
fi

LOG="${VINE_ASAN_LOG:-$BUILD/asan_${TARGET}.log}"
echo "[info] $TARGET --gtest_filter='$FILTER'  (QT_QPA_PLATFORM=$QT_QPA_PLATFORM)"
echo "[info] log: $LOG"
"$BIN" --gtest_filter="$FILTER" >"$LOG" 2>&1
STATUS=$?

# leaks_outside_scope_only — true when the ONLY findings are LeakSanitizer reports whose stacks do not
# mention $LEAK_SCOPE (a memory error or a failed test is never excused, and a non-zero exit with no leak
# report at all is not this function's to explain).
leaks_outside_scope_only() {
    grep -qE "ERROR: AddressSanitizer:|\[  FAILED  \]" "$LOG" && return 1
    grep -q "ERROR: LeakSanitizer" "$LOG" || return 1
    # Only the leak section is searched: the test output above it names test suites, which would match a
    # scope pattern like 'vn::vsg' and turn a report about someone else's allocation into a failure here.
    sed -n '/ERROR: LeakSanitizer/,$p' "$LOG" | grep -qE "$LEAK_SCOPE" && return 1
    return 0
}

# leaks_indirect_under_excused_only — true when the ONLY findings are INDIRECT LeakSanitizer entries.
# LSan prints an unmatched DIRECT root as "Direct leak of ...", so zero of those means every leaked root is
# one the suppression file already excused; an INDIRECT entry is then the same deliberate retention one hop
# down (LSan walks the pointers from the excused root). Those children are NOT hidden - their counts are
# printed below and the full report stays in $LOG - and a DIRECT leak, a memory error or a failed test still
# fails this run.
leaks_indirect_under_excused_only() {
    grep -qE "ERROR: AddressSanitizer:|\[  FAILED  \]" "$LOG" && return 1
    sed -n '/ERROR: LeakSanitizer/,$p' "$LOG" | grep -q "Direct leak of" && return 1
    sed -n '/ERROR: LeakSanitizer/,$p' "$LOG" | grep -q "Indirect leak of" || return 1
    return 0
}

# ASan writes its report to stderr and the process exits non-zero; gtest also
# uses a non-zero exit for test failures, so any non-zero means "not clean".
if [ "$STATUS" -ne 0 ]; then
    if [ -n "$LEAK_SCOPE" ] && [ "$LEAKS" = "1" ] && leaks_outside_scope_only; then
        echo "[warn] the run is non-zero only because of LeakSanitizer reports OUTSIDE '$LEAK_SCOPE':"
        sed -n '/ERROR: LeakSanitizer/,$p' "$LOG" | grep -E "^    #|^in " | sort -u | head -6 | sed 's/^/    /'
        echo "[warn] those are not this run's subject, but they are NOT hidden (see $LOG); a report whose"
        echo "       stack mentions '$LEAK_SCOPE' fails this run."
        echo "RESULT: PASS (scope '$LEAK_SCOPE' clean; leaks outside it reported above)"
        exit 0
    fi
    if [ "$LEAKS" = "1" ] && leaks_indirect_under_excused_only; then
        indirect_summary="$(sed -n '/ERROR: LeakSanitizer/,$p' "$LOG" | grep "Indirect leak of" | \
            awk '{ bytes += $4; chunks += $7; entries += 1 } END { printf "%d chunk(s) / %d byte(s) in %d entry(ies)", chunks, bytes, entries }')"
        echo "[warn] the remaining LeakSanitizer entries are INDIRECT: $indirect_summary"
        echo "[warn] - every leaked root is one the suppression file excuses (table below), and this rule"
        echo "       fails on any DIRECT leak; nothing here is hidden (full report: $LOG)."
        sed -n '/Suppressions used/,/^$/p' "$LOG" | sed 's/^/    /'
        echo "RESULT: PASS (indirect entries under excused roots)"
        exit 0
    fi
    echo "[info] --- AddressSanitizer / sanitizer report ---"
    sed -n '/ERROR: AddressSanitizer/,/^$/p' "$LOG" | head -40
    echo "[info] --- last test output ---"
    grep -v -E "^\[\+(2026|       OK)" "$LOG" | tail -15
    echo "RESULT: FAIL (exit $STATUS)"
    echo "[info] full log kept at $LOG"
    exit "$STATUS"
fi

echo "RESULT: PASS"
exit 0
