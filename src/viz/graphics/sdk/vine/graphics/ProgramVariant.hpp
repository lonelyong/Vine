#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <string>
#include <vector>

VN_GRAPHICS_NS_BEGIN

class GeometryDescription;
class MaterialDescription;

/**
 * @brief The VARIANT of a program: the defines that change what its text means.
 *
 * @details
 * - WHY A VARIANT AND NOT A SECOND PROGRAM. The engine's own stages gate declarations and code on names listed in
 *   their `#pragma import_defines` line (`VN_TEXTURED`, `VN_VERTEX_COLOR`, `VN_TEXCOORD_UV`, `VN_TEXCOORD_CUBE`,
 *   `VN_INSTANCED` - see the programs `BuiltinShaders.hpp` returns): one program TEXT is therefore several programs,
 *   and the difference is a compile-time one. Same identity, same revision - and a different picture when the defines
 *   are not the ones the drawable needs (a `vec2` attribute read where the text expects a direction, an instance
 *   matrix read where nothing feeds it).
 * - WHY THE SDK OWNS THE RULE (`G1` / `G12`: one concept, one owner). Both halves of it are the SDK's: the INPUTS are
 *   SDK values (`GeometryDescription`'s channels, `MaterialDescription`'s slots) and the OUTPUT names are the SDK's
 *   own stage text. A backend owns only how a set of names reaches its compiler - so two backends cannot disagree
 *   about which variant a drawable needs, and neither has to re-derive "three scalars is a cube" for itself.
 * - THE FOUR SWITCHES AND THE RULE THAT SETS THEM (see @ref variantOf):
 *   - `textured` - the material carries at least one texture slot. Binary on purpose: the variant then declares all
 *     the sampler slots and the stage short-circuits absent ones on the material block's flags, so per-slot bits
 *     would multiply the pipeline cache for no picture at all.
 *   - `vertex_color` - the geometry FEEDS the colour attribute (location 2). A backend derives no channel of its own,
 *     so every channel it sees is authored.
 *   - `cube_texcoord` - the texcoord channel's WIDTH is three scalars, i.e. a direction; two is a UV pair. The kind
 *     is not optional: a textured variant that does not name it is refused by the engine's stages, so a geometry
 *     with no texcoord channel still names the UV kind (a variant that samples nothing never reads the attribute).
 *   - `instanced` - the draw packet reads an instance stream (locations 12-15), which is a fact of the COMMAND.
 * - WHERE THE VALUE IS USED. In the program cache's key (a variant is one compile of the text), in the pipeline key
 *   (@ref bits is exactly the number `PipelineKey::variant` carries), and in the draw packet's resolution - which is
 *   why @ref variantOf is the one place the bits are derived: a warm-up and the frame that follows it cannot
 *   disagree about what to build.
 *
 * @note A backend that compiles the program text takes @ref defines; a backend that ships precompiled stages maps
 *       @ref bits to the stage it picks instead. Neither may derive the bits again.
 */
struct VN_GRAPHICS_API ProgramVariant {
    /// Bit of `textured` in @ref bits, and in `PipelineKey::variant`.
    static constexpr std::uint32_t kTexturedBit = 1U;
    /// Bit of `vertex_color` in @ref bits, and in `PipelineKey::variant`.
    static constexpr std::uint32_t kVertexColorBit = 2U;
    /// Bit of `cube_texcoord` in @ref bits, and in `PipelineKey::variant`.
    static constexpr std::uint32_t kCubeTexcoordBit = 4U;
    /// Bit of `instanced` in @ref bits, and in `PipelineKey::variant`.
    static constexpr std::uint32_t kInstancedBit = 8U;

    bool textured{ false };      ///< `VN_TEXTURED`: the material carries at least one texture slot.
    bool vertex_color{ false };  ///< `VN_VERTEX_COLOR`: the geometry feeds the colour channel (location 2).
    bool cube_texcoord{ false }; ///< `VN_TEXCOORD_CUBE`; false asks for `VN_TEXCOORD_UV` instead.
    bool instanced{ false };     ///< `VN_INSTANCED`: the call reads the per-instance model matrix at 12-15.

    /**
     * @brief Gets the number the program cache and the pipeline key carry for this variant.
     *
     * @details
     * - One bit per switch (see the `k*Bit` constants). A reader that compared two variants by anything else would
     *   have to know this mapping, which is what this type exists to avoid.
     *
     * @return The bit combination (`PipelineKey::variant`'s value for this variant).
     */
    [[nodiscard]] constexpr std::uint32_t bits() const noexcept
    {
        return (textured ? kTexturedBit : 0U) | (vertex_color ? kVertexColorBit : 0U) | (cube_texcoord ? kCubeTexcoordBit : 0U) |
               (instanced ? kInstancedBit : 0U);
    }

    /**
     * @brief Gets the define names a compile of this variant asks for.
     *
     * @details
     * - These are the names the engine's shipped stages gate on, which is why the SDK states them: a backend that
     *   compiles that text must hand exactly these over, and none of them is optional.
     * - EXACTLY ONE TEXCOORD KIND IS ALWAYS NAMED; the other three names appear when their switch is on.
     *
     * @return The names, in a stable order (`VN_TEXCOORD_*` first).
     */
    [[nodiscard]] std::vector<std::string> defines() const;

    /**
     * @brief Gets a one-line description of the variant, for diagnostics and test evidence.
     *
     * @return The same names `defines()` returns, comma-separated.
     */
    [[nodiscard]] std::string describe() const;
};

/** @brief Compares two variants (the identity is all four switches). */
[[nodiscard]] VN_GRAPHICS_API bool operator==(const ProgramVariant& left, const ProgramVariant& right) noexcept;

/** @brief Compares two variants. */
[[nodiscard]] VN_GRAPHICS_API bool operator!=(const ProgramVariant& left, const ProgramVariant& right) noexcept;

/**
 * @brief Gets the variant one scene draw needs: the SDK's rule (see the struct note).
 *
 * @details
 * - THE ONE PLACE THE FOUR BITS ARE DERIVED. A backend, a warm-up and a frame that follows it all ask this function,
 *   so what a cache compiled and what a draw needs cannot drift apart.
 * - The rule reads only SDK values, so it is answered without a device, a compiler or a backend.
 *
 * @param geometry  The geometry being drawn: its channel list states the colour and texcoord switches.
 * @param material  The material the drawable is shaded with, or null for the block's defaults (no texture slot).
 * @param instanced Whether the draw reads an instance stream (`DrawItem::instance_count != 0`).
 * @return The variant.
 */
[[nodiscard]] VN_GRAPHICS_API ProgramVariant variantOf(const GeometryDescription& geometry,
                                                       const MaterialDescription* material,
                                                       bool                       instanced) noexcept;

VN_GRAPHICS_NS_END
