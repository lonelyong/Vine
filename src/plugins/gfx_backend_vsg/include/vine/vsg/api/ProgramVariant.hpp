#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vine/vsg/vsg_global.hpp>

VN_VSG_NS_BEGIN

// Forward declarations: the two fact types this header NAMES but does not read (see variantOf). Including
// their definitions here would close a cycle - the tables key their program entries by this variant, so
// ContentFacts.hpp includes THIS header.
struct GeometryFacts;
struct MaterialFacts;

/**
 * @brief The VARIANT of a program: the defines that change what its text means.
 *
 * WHY A VARIANT AND NOT A SECOND PROGRAM. The engine's own stages gate declarations and code on names in
 * a `#pragma import_defines` line, and vsg delivers a define only when the source asks for it there:
 * one program text is therefore SEVERAL programs, and the difference is a compile-time one - the same
 * identity, the same revision, the same vertex layout, a different picture. A backend that compiled the
 * text once would draw one of the variants with the wrong meaning (a sampler bound where the text
 * expects none, a `vec2` attribute read where the text expects a direction) and no validation layer
 * would say a word.
 *
 * THE THREE SWITCHES ARE THE ENGINE'S OWN (see `builtin_forward.vert` / `builtin_gbuffer.frag`, whose
 * pragma lists these four names): they are what the engine's stages branch on, so they are what a
 * variant is.
 *
 * WHERE IT LIVES. In the pipeline key (`core::PipelineKey::variant` carries @ref bits - the core does
 * not know the engine's naming), in the scope's entry (the caller declares which variant each compiled
 * half serves), and in the DRAW: a pass computes a command's variant from its material and geometry
 * (see @ref variantOf) and only a half that serves it can draw the command.
 */
struct ProgramVariant
{
    bool diffuse_map{false};   ///< `VINE_DIFFUSE_MAP`: the material carries a texture to sample.
    bool vertex_color{false};  ///< `VINE_VERTEX_COLOR`: the geometry feeds the colour channel.
    bool cube_texcoord{false}; ///< `VINE_TEXCOORD_CUBE`; false asks for `VINE_TEXCOORD_UV` instead.

    /** @brief Gets the number the pipeline key carries for this variant (see core::PipelineKey::variant). */
    [[nodiscard]] std::uint32_t bits() const noexcept;

    /**
     * @brief Gets the define names a compile of this variant asks for.
     *
     * EXACTLY ONE KIND IS ALWAYS NAMED: the engine's stages refuse to build a variant that samples the
     * texcoord slot without stating what the slot IS (`#error a sampled texcoord slot needs one kind`),
     * so the kind is not an option - what is optional is whether the slot is sampled at all
     * (@ref diffuse_map).
     *
     * @return The names, one per line a compile adds (`VINE_TEXCOORD_UV`, `VINE_DIFFUSE_MAP`, ...).
     */
    [[nodiscard]] std::vector<std::string> defines() const;

    /** @brief Gets a one-line description of the variant, for diagnostics. */
    [[nodiscard]] std::string describe() const;
};

/** @brief Compares two variants (the identity is all three switches). */
[[nodiscard]] bool operator==(const ProgramVariant& left, const ProgramVariant& right) noexcept;

/** @brief Compares two variants. */
[[nodiscard]] bool operator!=(const ProgramVariant& left, const ProgramVariant& right) noexcept;

/**
 * @brief Gets the variant a drawable of @p geometry and @p material is compiled for: the ENGINE's rule.
 *
 *   * the texcoord KIND follows the geometry's channel WIDTH - three scalars per vertex is a direction
 *     and takes the cube sampler, two is a UV pair (the engine's stages say so; the backend does not
 *     get to guess, and a texture whose kind disagrees with the channel gives way, not the data);
 *   * `VINE_DIFFUSE_MAP` turns on when the MATERIAL has a texture. A material without one takes the
 *     variant with no sampler and no fetch at all, which is the variant the engine's own stages gate
 *     their sampler on;
 *   * `VINE_VERTEX_COLOR` turns on when the geometry AUTHORS the colour channel: a derived one (the
 *     backend's own white carrier) is not an authored colour, so multiplying by it would only cost.
 *
 * @param material The material the drawable is shaded with (its texture is the switch's input).
 * @param geometry The geometry being drawn (its fed channels are the other two).
 * @return The variant.
 */
[[nodiscard]] ProgramVariant variantOf(const MaterialFacts& material, const GeometryFacts& geometry) noexcept;

VN_VSG_NS_END
