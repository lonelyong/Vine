#!/usr/bin/env bash
# vine_shader_check.sh — Offline validation of every embedded GLSL shader.
#
# Shaders are compiled at RUNTIME (vsg ShaderCompiler / glslang), so a syntax
# error, an unbalanced variant #if, or a bad binding would only show up as a
# "ShaderFallback" warning in a live session — and possibly as a wrong picture.
# This gate moves that failure mode in front of the commit, without a GPU:
#
#   1. COMPILE  every shader under a shaders/ directory with glslangValidator,
#               once per combination of the variant defines the backend may set
#               (VINE_VERTEX_COLOR, VINE_DIFFUSE_MAP, VINE_FLAT) crossed with
#               each texcoord kind (VINE_TEXCOORD_UV / VINE_TEXCOORD_CUBE, of
#               which the backend sets exactly one). A shader that uses
#               `#ifdef VINE_VERTEX_COLOR` therefore has to compile in BOTH
#               variants in the same run, which is what the runtime does per
#               drawable. Unused defines are inert in GLSL, so the matrix needs
#               no manifest to stay in sync with the sources.
#   2. SYNC     for every generated header (cmake/VineShaders.cmake writes them
#               into <build>/generated/vine/...), verify that the recorded
#               SHA-256 prefix and byte count still match the file on disk, so an
#               embedded copy can never silently drift from its source. Skipped,
#               with a notice, when the header is not built yet.
#
# Layout is checked too: the shader inventory (cmake/VineShaders.cmake) must cover
# every file under a shaders/ directory, so adding a .vert/.frag without embedding
# it is a failure rather than a dead file.
#
# Usage:
#   scripts/vine_shader_check.sh [BUILD_DIR]      BUILD_DIR defaults to <root>/build
#
# Env overrides:
#   GLSLANG_VALIDATOR      Validator binary (default: glslangValidator on PATH).
#   VINE_SHADER_CHECK_STAGE  glslang target env (default: vulkan1.1).
#
# Exit code 0 when every shader compiles and every embedded copy is in sync.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="${1:-$ROOT/build}"

VALIDATOR="${GLSLANG_VALIDATOR:-$(command -v glslangValidator || true)}"
TARGET_ENV="${VINE_SHADER_CHECK_STAGE:-vulkan1.1}"

INVENTORY="$ROOT/cmake/VineShaders.cmake"

# The variant defines the backend can set on a compiled stage. Keep in sync with
# the define names used by the shaders themselves (the compile matrix below is
# what catches a mismatch).
VARIANT_DEFINES=(VINE_VERTEX_COLOR VINE_DIFFUSE_MAP VINE_FLAT)
# The texcoord KIND is not an on/off flag: the backend sets EXACTLY ONE of these
# on every content variant (the geometry's texcoord width decides which), and the
# shaders reject a sampled slot whose kind is unstated (#error) rather than
# defaulting to one. The matrix therefore runs every flag combination once per
# kind — the shapes a real variant can have — instead of the full cross product,
# which would keep generating states the sources now refuse on purpose.
VARIANT_KINDS=(VINE_TEXCOORD_UV VINE_TEXCOORD_CUBE)

FAILED=0

if [ -z "$VALIDATOR" ] || [ ! -x "$VALIDATOR" ]; then
    echo "[FAIL] glslangValidator not found; install it (e.g. 'apt install glslang-tools')"
    echo "       or set GLSLANG_VALIDATOR=/path/to/glslangValidator"
    echo "RESULT: FAIL — no GLSL validator available."
    exit 1
fi
echo "[info] validator: $VALIDATOR ($("$VALIDATOR" --version 2>&1 | head -1))"
echo "[info] target env: $TARGET_ENV"

# ---- Collect the shader sources ---------------------------------------------
mapfile -t SHADERS < <(find "$ROOT/src" -path '*/shaders/*' -type f \( -name '*.vert' -o -name '*.frag' \
    -o -name '*.comp' -o -name '*.geom' -o -name '*.tesc' -o -name '*.tese' \) | sort)

if [ "${#SHADERS[@]}" -eq 0 ]; then
    echo "[FAIL] no shader sources found under src/**/shaders/"
    FAILED=1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---- 1. Compile every shader for every define combination -------------------
for shader in "${SHADERS[@]}"; do
    rel="${shader#"$ROOT"/}"
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
            # writes vert.spv / frag.spv into the CURRENT directory.
            if "$VALIDATOR" -V --target-env "$TARGET_ENV" "${args[@]}" -o "$WORK/stage.$mask.$kind.spv" "$shader" >"$log" 2>&1; then
                echo "[PASS] $rel [$variant]"
            else
                echo "[FAIL] $rel [$variant]"
                sed 's/^/       /' "$log"
                FAILED=1
            fi
        done
    done
done

# ---- 2. Every shader file is embedded ---------------------------------------
for shader in "${SHADERS[@]}"; do
    rel="${shader#"$ROOT"/}"
    if ! grep -qF "$(basename "$shader")" "$INVENTORY"; then
        echo "[FAIL] $rel is not listed in cmake/VineShaders.cmake"
        FAILED=1
    fi
done

# ---- 3. Every embedded copy still matches its source ------------------------
# The header list comes from the MANIFEST, not from a glob of the build tree: a generated header whose
# declaring block was removed stays on disk (nothing regenerates or deletes it), and a glob would keep
# checking an owner that no longer exists — measured: after the backend's shaders moved to the SDK, the
# stale vine/vsg/EmbeddedShaders.hpp made this check fail on a file that is not in the tree any more.
headers=()
while read -r declared; do
    [ -n "$declared" ] && headers+=( "$BUILD/generated/$declared" )
done < <(grep -A 1 'v_declare_embedded_shaders' "$INVENTORY" | sed -n 's/^ *OUTPUT \(.*\)$/\1/p')
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
            actual_hash=$(sha256sum "$source" | cut -c1-16)
            actual_bytes=$(stat -c%s "$source")
            if [ "$actual_hash" != "$hash" ] || [ "$actual_bytes" != "$bytes" ]; then
                echo "[FAIL] $name: embedded copy is stale (embedded $hash/$bytes bytes, file $actual_hash/$actual_bytes bytes)"
                echo "       rebuild to re-embed it"
                FAILED=1
            else
                echo "[PASS] $name embedded byte-for-byte"
            fi
        done < <(sed -n 's/^    { "\([^"]*\)", [^,]*, "\([^"]*\)", \([0-9]*\) },$/\1 \2 \3/p' "$header")
    done
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "RESULT: PASS — ${#SHADERS[@]} shader(s) compile and every embedded copy is in sync."
    exit 0
else
    echo "RESULT: FAIL — see messages above."
    exit 1
fi
