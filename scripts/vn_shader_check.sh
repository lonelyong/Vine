#!/usr/bin/env bash
# vn_shader_check.sh — Offline validation of every embedded GLSL shader.
#
# Shaders are compiled at RUNTIME (vsg ShaderCompiler / glslang), so a syntax
# error, an unbalanced variant #if, or a bad binding would only show up as a
# "ShaderFallback" warning in a live session — and possibly as a wrong picture.
# This gate moves that failure mode in front of the commit, without a GPU:
#
#   0. EXPAND   replace every `#include "vine_params.glsl"` line with the
#               fragment's text using the SAME script the build uses
#               (cmake/VineEmbedShaders.cmake, VN_EXPAND_ONLY). What this gate
#               then compiles and hashes is exactly what the build embeds — one
#               expansion implementation, so the two cannot disagree.
#   1. COMPILE  every shader under a shaders/ directory with glslangValidator,
#               once per combination of the variant defines the backend may set
#               (VN_VERTEX_COLOR, VN_DIFFUSE_MAP, VN_FLAT) crossed with
#               each texcoord kind (VN_TEXCOORD_UV / VN_TEXCOORD_CUBE, of
#               which the backend sets exactly one). A shader that uses
#               `#ifdef VN_VERTEX_COLOR` therefore has to compile in BOTH
#               variants in the same run, which is what the runtime does per
#               drawable. Unused defines are inert in GLSL, so the matrix needs
#               no manifest to stay in sync with the sources.
#   2. SYNC     for every generated header (cmake/VineShaders.cmake writes them
#               into <build>/generated/vine/...), verify that the recorded
#               SHA-256 prefix and byte count still match the EXPANDED source, so
#               an embedded copy can never silently drift from its source. Skipped,
#               with a notice, when the header is not built yet.
#
# Layout is checked too: the shader inventory (cmake/VineShaders.cmake) must cover
# every file under a shaders/ directory — stages under SOURCES, `#include`
# fragments under INCLUDES — so adding a .vert/.frag/.glsl without declaring it is
# a failure rather than a dead file, and a fragment no shader includes is one too.
#
# Usage:
#   scripts/vn_shader_check.sh [BUILD_DIR]      BUILD_DIR defaults to <root>/build
#
# Env overrides:
#   GLSLANG_VALIDATOR      Validator binary (default: glslangValidator on PATH).
#   VN_SHADER_CHECK_STAGE  glslang target env (default: vulkan1.1).
#   CMAKE_BIN              cmake binary (default: cmake on PATH).
#
# Exit code 0 when every shader compiles and every embedded copy is in sync.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="${1:-$ROOT/build}"

VALIDATOR="${GLSLANG_VALIDATOR:-$(command -v glslangValidator || true)}"
TARGET_ENV="${VN_SHADER_CHECK_STAGE:-vulkan1.1}"
CMAKE="${CMAKE_BIN:-$(command -v cmake || true)}"

INVENTORY="$ROOT/cmake/VineShaders.cmake"

# The variant defines the backend can set on a compiled stage. Keep in sync with
# the define names used by the shaders themselves (the compile matrix below is
# what catches a mismatch).
VARIANT_DEFINES=(VN_VERTEX_COLOR VN_DIFFUSE_MAP VN_FLAT VN_INSTANCED)
# The texcoord KIND is not an on/off flag: the backend sets EXACTLY ONE of these
# on every content variant (the geometry's texcoord width decides which), and the
# shaders reject a sampled slot whose kind is unstated (#error) rather than
# defaulting to one. The matrix therefore runs every flag combination once per
# kind — the shapes a real variant can have — instead of the full cross product,
# which would keep generating states the sources now refuse on purpose.
VARIANT_KINDS=(VN_TEXCOORD_UV VN_TEXCOORD_CUBE)

FAILED=0

if [ -z "$VALIDATOR" ] || [ ! -x "$VALIDATOR" ]; then
    echo "[FAIL] glslangValidator not found; install it (e.g. 'apt install glslang-tools')"
    echo "       or set GLSLANG_VALIDATOR=/path/to/glslangValidator"
    echo "RESULT: FAIL — no GLSL validator available."
    exit 1
fi
if [ -z "$CMAKE" ] || [ ! -x "$CMAKE" ]; then
    echo "[FAIL] cmake not found; the include fragments are expanded with cmake/VineEmbedShaders.cmake"
    echo "       (the same script the build uses), so the gate needs it: set CMAKE_BIN=/path/to/cmake"
    echo "RESULT: FAIL — no cmake available."
    exit 1
fi
echo "[info] validator: $VALIDATOR ($("$VALIDATOR" --version 2>&1 | head -1))"
echo "[info] target env: $TARGET_ENV"

# ---- Collect the shader sources and the included fragments -------------------
mapfile -t SHADERS < <(find "$ROOT/src" -path '*/shaders/*' -type f \( -name '*.vert' -o -name '*.frag' \
    -o -name '*.comp' -o -name '*.geom' -o -name '*.tesc' -o -name '*.tese' \) | sort)
mapfile -t FRAGMENTS < <(find "$ROOT/src" -path '*/shaders/*' -type f -name '*.glsl' | sort)

if [ "${#SHADERS[@]}" -eq 0 ]; then
    echo "[FAIL] no shader sources found under src/**/shaders/"
    FAILED=1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---- 0. Expand every shader's #include fragments -----------------------------
# One implementation, shared with the build: the text this gate compiles and hashes is the text the
# embedded header carries, so "compiles here" means "compiles as shipped".
EXPANDED_DIR="$WORK/expanded"
mkdir -p "$EXPANDED_DIR"
declare -A EXPANDED   # <source path relative to ROOT> -> expanded copy
for shader in "${SHADERS[@]}"; do
    rel="${shader#"$ROOT"/}"
    out="$EXPANDED_DIR/${rel//\//__}"
    if "$CMAKE" "-DOUT=$out" "-DSOURCES=$shader" -DVN_EXPAND_ONLY=ON \
        -P "$ROOT/cmake/VineEmbedShaders.cmake" >"$WORK/expand.log" 2>&1; then
        EXPANDED["$rel"]="$out"
    else
        echo "[FAIL] $rel: #include expansion failed"
        sed 's/^/       /' "$WORK/expand.log"
        FAILED=1
    fi
done

# ---- 1. Compile every shader for every define combination -------------------
for shader in "${SHADERS[@]}"; do
    rel="${shader#"$ROOT"/}"
    expanded="${EXPANDED[$rel]:-}"
    if [ -z "$expanded" ]; then
        continue   # already reported by the expansion step
    fi
    stage="${shader##*.}"
    # Bit combinations of the on/off defines: 0 = none, 1 = ...COMBOS binaries.
    count=$((1 << ${#VARIANT_DEFINES[@]}))
    for ((mask = 0; mask < count; ++mask)); do
        flags=()
        names=""
        for ((bit = 0; bit < ${#VARIANT_DEFINES[@]}; ++bit)); do
            if ((mask & (1 << bit))); then
                flags+=("-D${VARIANT_DEFINES[$bit]}=1")
                names="${names:+$names,}${VARIANT_DEFINES[$bit]}"
            fi
        done
        # Exactly one texcoord kind, always: that is what a variant carries.
        for kind in "${VARIANT_KINDS[@]}"; do
            args=("${flags[@]}" "-D${kind}=1")
            variant="${names:+$names,}$kind"
            log="$WORK/$(basename "$shader").$mask.$kind.log"
            # -o keeps the validator's .spv output in the temp dir: without it, glslang
            # writes vert.spv / frag.spv into the CURRENT directory. -S names the stage the
            # expanded copy belongs to (its name is the source path, not a stage suffix).
            if "$VALIDATOR" -V --target-env "$TARGET_ENV" -S "$stage" "${args[@]}" -o "$WORK/stage.$mask.$kind.spv" "$expanded" >"$log" 2>&1; then
                echo "[PASS] $rel [$variant]"
            else
                echo "[FAIL] $rel [$variant]"
                sed 's/^/       /' "$log"
                FAILED=1
            fi
        done
    done
done

# ---- 2. Every shader file and fragment is declared --------------------------
for shader in "${SHADERS[@]}"; do
    rel="${shader#"$ROOT"/}"
    if ! grep -qF "$(basename "$shader")" "$INVENTORY"; then
        echo "[FAIL] $rel is not listed in cmake/VineShaders.cmake"
        FAILED=1
    fi
done

# A fragment is declared under INCLUDES (the generated header must depend on it) and has to be
# INCLUDED by at least one stage: a fragment nobody includes is a dead file, the same finding the
# inventory check makes for a stage that is never embedded.
for fragment in "${FRAGMENTS[@]}"; do
    rel="${fragment#"$ROOT"/}"
    if ! grep -qF "$(basename "$fragment")" "$INVENTORY"; then
        echo "[FAIL] $rel is not listed under INCLUDES in cmake/VineShaders.cmake"
        FAILED=1
    fi
    if ! grep -rqE "^#include \"$(basename "$fragment")\"" "$ROOT/src"; then
        echo "[FAIL] $rel is included by no shader"
        FAILED=1
    fi
done

# ---- 3. Every embedded copy still matches its EXPANDED source ---------------
# The header list comes from the MANIFEST, not from a glob of the build tree: a generated header whose
# declaring block was removed stays on disk (nothing regenerates or deletes it), and a glob would keep
# checking an owner that no longer exists — measured: after the backend's shaders moved to the SDK, the
# stale vine/vsg/EmbeddedShaders.hpp made this check fail on a file that is not in the tree any more.
#
# The hash and byte count are the expanded text's (see VineEmbedShaders.cmake): a fragment edit changes
# them even though the shader file on disk is untouched, which is exactly the drift this step is for.
headers=()
while read -r declared; do
    [ -n "$declared" ] && headers+=( "$BUILD/generated/$declared" )
done < <(grep -A 1 'vn_declare_embedded_shaders' "$INVENTORY" | sed -n 's/^ *OUTPUT \(.*\)$/\1/p')
if [ "${#headers[@]}" -eq 0 ] || [ ! -f "${headers[0]}" ]; then
    echo "[SKIP] no generated shader header yet ($BUILD/generated/vine/...); build once to check the embedding"
else
    for header in "${headers[@]}"; do
        [ -f "$header" ] || continue
        entries=$(grep -c '^    { "' "$header")
        echo "[info] $header: $entries entry/entries"
        while read -r name hash bytes; do
            source="$ROOT"
            # The entry only carries the file name: find the source it came from.
            source=$(find "$ROOT/src" -type f -name "$name" -print -quit)
            if [ -z "$source" ]; then
                echo "[FAIL] $name: embedded but not found under src/"
                FAILED=1
                continue
            fi
            rel="${source#"$ROOT"/}"
            expanded="${EXPANDED[$rel]:-}"
            if [ -z "$expanded" ]; then
                expanded="$EXPANDED_DIR/${rel//\//__}.sync"
                if ! "$CMAKE" "-DOUT=$expanded" "-DSOURCES=$source" -DVN_EXPAND_ONLY=ON \
                    -P "$ROOT/cmake/VineEmbedShaders.cmake" >"$WORK/expand.log" 2>&1; then
                    echo "[FAIL] $name: #include expansion failed"
                    sed 's/^/       /' "$WORK/expand.log"
                    FAILED=1
                    continue
                fi
            fi
            actual_hash=$(sha256sum "$expanded" | cut -c1-16)
            actual_bytes=$(stat -c%s "$expanded")
            if [ "$actual_hash" != "$hash" ] || [ "$actual_bytes" != "$bytes" ]; then
                echo "[FAIL] $name: embedded copy is stale (embedded $hash/$bytes bytes, expanded source $actual_hash/$actual_bytes bytes)"
                echo "       rebuild to re-embed it"
                FAILED=1
            else
                echo "[PASS] $name embedded byte-for-byte (its #include fragments expanded)"
            fi
        done < <(sed -n 's/^    { "\([^"]*\)", [^,]*, "\([^"]*\)", \([0-9]*\) },$/\1 \2 \3/p' "$header")
    done
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "RESULT: PASS — ${#SHADERS[@]} shader(s) compile and every embedded copy is in sync (${#FRAGMENTS[@]} fragment(s) inlined)."
    exit 0
else
    echo "RESULT: FAIL — see messages above."
    exit 1
fi
