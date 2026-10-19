#pragma once

#include "graphics_global.hpp"

#include <cstdint>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief How a sampled image is READ: filter, wrap, mip and anisotropy state, independent of the image.
 *
 * @details
 * - Vulkan keeps the sampler out of the image (a `VkSampler` is bound beside a `VkImageView`), and this value is
 *   the same separation at the SDK's level: one texture can be read with one state by a pass that previews it and
 *   another state by a pass that tiles it, without re-uploading anything.
 * - The defaults are the common scene-texture state - linear min/mag, linear mip interpolation, repeat on every
 *   axis, no anisotropy, no LOD bias - which is also what a pass input that states nothing keeps using (the
 *   backend's own input sampler, so an existing declaration does not silently change).
 * - The fields are deliberately the ones a 3D renderer's sampler description carries (Filament's
 *   `SamplerParams`, three.js' texture filters/wrap modes, vsg's `vsg::Sampler`): `min_filter` / `mag_filter`,
 *   `mipmap_mode`, the three address modes, `anisotropy`, `lod_bias` and the LOD range. Compare samplers
 *   (hardware depth comparison) are not part of this slice: the pass depth path reads exact depths, and a
 *   compare sampler would need an ABI of its own (see `.ai/design/graphics-image-source.md`).
 * - A value, not an object: two equal `Sampler`s are the same state, so a cache can key on it. The backend
 *   clamps `anisotropy` to what the device reports and maps every field to its `VkSamplerCreateInfo` spelling;
 *   the SDK stores what the host stated.
 *
 * @note
 * - The border colour is only read for `AddressMode::ClampToBorder`: the other modes never produce a border
 *   texel, and Vulkan leaves the field unused for them.
 * - A `Sampler` doubles as a cache key (see the backend's image cache), so its fields are compared by value:
 *   pass finite numbers. A NaN compares unequal to itself and would miss the cache on every frame.
 */
struct VN_GRAPHICS_API Sampler
{
    /** @brief How a texel is filtered when it is magnified or minified. */
    enum class Filter : std::uint8_t
    {
        Nearest, ///< The nearest texel (blocky, exact).
        Linear,  ///< Bilinear interpolation (smooth).
    };

    /** @brief How two mip levels are blended. */
    enum class MipmapMode : std::uint8_t
    {
        Nearest, ///< One level is chosen.
        Linear,  ///< Two levels are interpolated.
    };

    /** @brief What a coordinate outside [0, 1] reads. */
    enum class AddressMode : std::uint8_t
    {
        Repeat,            ///< Tiles the image.
        MirroredRepeat,    ///< Tiles it with every other tile flipped.
        ClampToEdge,       ///< Repeats the edge texel.
        ClampToBorder,     ///< Reads @ref border_color outside the image.
        MirrorClampToEdge, ///< Mirrors once, then repeats the edge texel.
    };

    /** @brief The value `ClampToBorder` reads outside the image. */
    enum class BorderColor : std::uint8_t
    {
        TransparentBlack, ///< (0, 0, 0, 0).
        OpaqueBlack,      ///< (0, 0, 0, 1).
        OpaqueWhite,      ///< (1, 1, 1, 1).
    };

    Filter      min_filter{Filter::Linear};        ///< Filter for minified texels.
    Filter      mag_filter{Filter::Linear};        ///< Filter for magnified texels.
    MipmapMode  mipmap_mode{MipmapMode::Linear};    ///< How mip levels are blended.
    AddressMode address_u{AddressMode::Repeat};     ///< Wrap mode for the u axis.
    AddressMode address_v{AddressMode::Repeat};     ///< Wrap mode for the v axis.
    AddressMode address_w{AddressMode::Repeat};     ///< Wrap mode for the w axis (3D/cube sampling).
    BorderColor border_color{BorderColor::TransparentBlack};  ///< Value read by ClampToBorder.
    float       anisotropy{1.0F};  ///< Requested anisotropy (1 = off; the backend clamps it to the device limit).
    float       lod_bias{0.0F};    ///< Bias added to the computed level of detail.
    float       min_lod{0.0F};     ///< Lowest level of detail the sampler may read.
    float       max_lod{1000.0F};  ///< Highest level of detail the sampler may read (Vulkan's clamp value).

    /** @brief States whether two sampler descriptions are the same state.
     *
     * @param other The other description.
     * @return true when every field agrees.
     */
    friend bool operator==(const Sampler&, const Sampler&) = default;
};

VN_GRAPHICS_NS_END
