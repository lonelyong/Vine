#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/MaterialBase.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief The VARIANT of a program: the defines that change what its text means.
 *
 * @details
 * - WHY A VARIANT AND NOT A SECOND PROGRAM. The engine's own stages gate declarations and code on names listed in
 *   their `#pragma import_defines` line (`VN_VERTEX_COLOR`, `VN_TEXTURED`, `VN_TEXCOORD_UV`, `VN_TEXCOORD_CUBE`,
 *   `VN_INSTANCED` - see `builtin_forward.vert`): one program text is therefore SEVERAL programs, and the difference
 *   is a compile-time one. The same identity, the same revision - and a different picture if the definitions are
 *   not the ones the drawable needs (a `vec2` attribute read where the text expects a direction, an instance matrix
 *   read where nothing feeds it).
 * - THE FOUR SWITCHES ARE THE ENGINE'S OWN, and the rules that turn a drawable into them are the ENGINE's too (this
 *   re-states the reference backend's rules; the plugins share no header and must agree by contract, not by include):
 *   - `VN_TEXTURED` - the material carries at least one texture slot. Binary on purpose: the variant then declares
 *     all five samplers and the shader short-circuits absent slots on the material block's flags, so five per-slot
 *     bits would multiply the pipeline cache for no picture at all.
 *   - `VN_VERTEX_COLOR` - the geometry FEEDS the colour attribute (location 2). This backend derives no channel of
 *     its own, so every channel it sees is authored.
 *   - the texcoord KIND follows the geometry's texcoord channel WIDTH: three scalars is a direction (cube sampler),
 *     two is a UV pair. The kind is not optional - the engine's stages refuse to build a textured variant that does
 *     not name what the slot is - so a geometry with no texcoord channel still names `VN_TEXCOORD_UV` (a variant
 *     that samples nothing never reads the attribute).
 *   - `VN_INSTANCED` - the drawing call reads an instance stream (locations 12-15), which is a fact of the COMMAND.
 * - WHERE IT LIVES. In the program cache's key (a variant is one compile of the text), in the pipeline key (the
 *   rendered result differs), and in the draw: `variantOf` is the one place those bits are derived, so a warm-up and
 *   the frame that follows it cannot disagree about what to build (see ContentDraw's `planCommand`).
 */
struct VN_BACKEND_VK_API ProgramVariant {
    bool textured{ false };      // `VN_TEXTURED`: the material carries at least one texture slot.
    bool vertex_color{ false };  // `VN_VERTEX_COLOR`: the geometry feeds the colour channel (location 2).
    bool cube_texcoord{ false }; // `VN_TEXCOORD_CUBE`; false asks for `VN_TEXCOORD_UV` instead.
    bool instanced{ false };     // `VN_INSTANCED`: the call reads the per-instance model matrix at 12-15.

    /**
     * @brief Gets the number the program cache and the pipeline key carry for this variant.
     *
     * @return One bit per switch (`textured` 1, `vertex_color` 2, `cube_texcoord` 4, `instanced` 8).
     */
    [[nodiscard]] std::uint32_t bits() const noexcept;

    /**
     * @brief Gets the define names a compile of this variant asks for.
     *
     * @details
     * - EXACTLY ONE TEXCOORD KIND IS ALWAYS NAMED (see the struct note); the other three names appear when the
     *   switch is on.
     *
     * @return The names, in a stable order (`VN_TEXCOORD_*` first).
     */
    [[nodiscard]] std::vector<std::string> defines() const;

    /** @brief Gets a one-line description of the variant, for diagnostics and test evidence. */
    [[nodiscard]] std::string describe() const;
};

/** @brief Compares two variants (the identity is all four switches). */
[[nodiscard]] VN_BACKEND_VK_API bool operator==(const ProgramVariant& left, const ProgramVariant& right) noexcept;

/** @brief Compares two variants. */
[[nodiscard]] VN_BACKEND_VK_API bool operator!=(const ProgramVariant& left, const ProgramVariant& right) noexcept;

/**
 * @brief Gets the variant one scene drawing call is compiled for: the ENGINE's rule (see the struct note).
 *
 * @param geometry  The geometry being drawn: its channel list states the colour and texcoord switches.
 * @param material  The material the drawable is shaded with, or null for the block's defaults (no texture slot).
 * @param instanced Whether the drawing call reads an instance stream (`RenderCommand::instance_count != 0`).
 * @return The variant.
 */
[[nodiscard]] VN_BACKEND_VK_API ProgramVariant variantOf(const vn::graphics::GeometryDescription& geometry,
                                                         const vn::graphics::MaterialDescription* material,
                                                         bool                                     instanced) noexcept;

VN_BACKEND_VK_NS_END
