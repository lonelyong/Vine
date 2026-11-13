#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>
#include <optional>
#include <utility>

#include <volk.h>

#include <vine/graphics/DepthRange.hpp>
#include <vine/graphics/PolygonMode.hpp>
#include <vine/graphics/RenderState.hpp>
#include <vine/graphics/Topology.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief THE ONE PLACE where the SDK's render-state vocabulary becomes a Vulkan spelling.
 *
 * @details
 * - WHY A TABLE AND NOT INLINE SWITCHES. The same three facts - how a depth comparison, a blend factor and a cull mode
 *   are spelled on a device - are needed by the pipeline builder and by anything that explains a picture; writing the
 *   switches where they are used is how a second backend ends up with two answers for one state. This header is the vk
 *   backend's half of the seam (the same role vsg's `RenderStateMapper` plays for that backend), and it is PURE: no
 *   device, no allocation, so every row is testable without a GPU.
 * - TWO CONVENTIONS DECIDE THE NON-OBVIOUS ROWS, and both are stated here rather than at a call site:
 *   - REVERSE-Z. The engine renders with the far plane at 0 and the near plane at 1 (see Camera), so the SDK's
 *     `CompareOp::Less` ("closer passes") is Vulkan's `GREATER`: a nearer fragment has the LARGER stored value.
 *     Mapping it literally would make every depth test pass backwards.
 *   - THE SDK'S FRONT FACE IS COUNTER-CLOCKWISE IN A RIGHT-HANDED WORLD, and this backend flips Y in the projection
 *     (`buildViewBlock`) so the picture is upright in Vulkan's top-left framebuffer space. That flip turns the SDK's
 *     CCW into a CLOCKWISE polygon in framebuffer coordinates, which is why @ref kVkFrontFace is CLOCKWISE - the same
 *     mapping the vsg backend states (its `perspective` negates the Y term).
 * - A value this header does not know (a new enumerator) falls back to the least surprising choice and is meant to be
 *   added here: the switches name every SDK value, so adding one makes the compiler point at this file.
 */
struct StateMapping {
    /**
     * @brief Gets the Vulkan front face that means the SDK's counter-clockwise rule (see the file note).
     *
     * @details
     * - A single constant rather than a function: the SDK has ONE front-face convention, and a backend that flipped it
     *   per draw would be a second convention nobody asked for.
     */
    static constexpr VkFrontFace kFrontFace = VK_FRONT_FACE_CLOCKWISE;

    /**
     * @brief Gets the Vulkan comparison for an SDK depth comparison (reverse-Z, see the file note).
     *
     * @param op The SDK comparison.
     * @return The Vulkan comparison.
     */
    [[nodiscard]] static constexpr VkCompareOp compareOp(vn::graphics::CompareOp op) noexcept
    {
        using vn::graphics::CompareOp;
        switch (op) {
        case CompareOp::Never: return VK_COMPARE_OP_NEVER;
        case CompareOp::Less: return VK_COMPARE_OP_GREATER;
        case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
        case CompareOp::LessEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::Greater: return VK_COMPARE_OP_LESS;
        case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::GreaterEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
        }
        return VK_COMPARE_OP_GREATER;
    }

    /**
     * @brief Gets the Vulkan blend factor for an SDK blend factor.
     *
     * @param factor The SDK factor.
     * @return The Vulkan factor (ONE for a value the switch does not know, which is a no-op blend).
     */
    [[nodiscard]] static constexpr VkBlendFactor blendFactor(vn::graphics::BlendFactor factor) noexcept
    {
        using vn::graphics::BlendFactor;
        switch (factor) {
        case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
        case BlendFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        }
        return VK_BLEND_FACTOR_ONE;
    }

    /**
     * @brief Gets the Vulkan cull mask for an SDK cull mode.
     *
     * @param mode The SDK cull mode (Front means the SDK's counter-clockwise faces).
     * @return The Vulkan cull mask, paired with @ref kFrontFace.
     */
    [[nodiscard]] static constexpr VkCullModeFlags cullMode(vn::graphics::CullMode mode) noexcept
    {
        using vn::graphics::CullMode;
        switch (mode) {
        case CullMode::None: return VK_CULL_MODE_NONE;
        case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
        case CullMode::Back: return VK_CULL_MODE_BACK_BIT;
        }
        return VK_CULL_MODE_NONE;
    }

    /**
     * @brief Gets the Vulkan polygon mode for an SDK rasterisation mode.
     *
     * @param mode The SDK polygon mode.
     * @return The Vulkan polygon mode.
     */
    [[nodiscard]] static constexpr VkPolygonMode polygonMode(vn::graphics::PolygonMode mode) noexcept
    {
        using vn::graphics::PolygonMode;
        switch (mode) {
        case PolygonMode::Fill: return VK_POLYGON_MODE_FILL;
        case PolygonMode::Line: return VK_POLYGON_MODE_LINE;
        case PolygonMode::Point: return VK_POLYGON_MODE_POINT;
        }
        return VK_POLYGON_MODE_FILL;
    }

    /**
     * @brief Gets the Vulkan sample count for a target's samples per pixel.
     *
     * @details
     * - A TARGET'S SAMPLE COUNT IS A NUMBER, not a device flag: the SDK states samples per pixel (`TargetShape::samples`),
     *   while Vulkan spells the same fact as a `VkSampleCountFlagBits` whose enumerator happens to be that number. The
     *   mapping is stated once, here, because the render pass and the pipeline must read the same answer - two
     *   spellings would be a render pass and a pipeline that disagree about how many samples they draw.
     * - A count Vulkan has no flag for (0, or a number that is not one of its counts) answers 1: the request is clamped
     *   to what the device offers before it reaches this table (see `Device::ensureTarget`), so an unknown value is a
     *   bug upstream, and drawing single-sampled is the only safe reading of it.
     *
     * @param samples Samples per pixel, as the target's shape states them.
     * @return The Vulkan sample count.
     */
    [[nodiscard]] static constexpr VkSampleCountFlagBits sampleCount(std::uint32_t samples) noexcept
    {
        switch (samples) {
        case 2U: return VK_SAMPLE_COUNT_2_BIT;
        case 4U: return VK_SAMPLE_COUNT_4_BIT;
        case 8U: return VK_SAMPLE_COUNT_8_BIT;
        case 16U: return VK_SAMPLE_COUNT_16_BIT;
        case 32U: return VK_SAMPLE_COUNT_32_BIT;
        case 64U: return VK_SAMPLE_COUNT_64_BIT;
        default: return VK_SAMPLE_COUNT_1_BIT;
        }
    }

    /**
     * @brief Gets the Vulkan topology for an SDK topology.
     *
     * @param topology The SDK topology.
     * @return The Vulkan topology (triangles for a value this table does not know).
     */
    [[nodiscard]] static constexpr VkPrimitiveTopology topology(vn::graphics::Topology topology) noexcept
    {
        using vn::graphics::Topology;
        switch (topology) {
        case Topology::Triangles: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case Topology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case Topology::TriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case Topology::Points: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case Topology::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case Topology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        }
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }

    /**
     * @brief Gets the viewport's min/max depth for a pass' depth band.
     *
     * @details
     * - `DepthRange::min` is the FAR end and `max` the NEAR end, which is exactly Vulkan's `minDepth`/`maxDepth` pair
     *   under reverse-Z: the viewport transform maps NDC z = 0 to `minDepth` and z = 1 to `maxDepth`, and reverse-Z
     *   puts the far plane at 0.
     * - No range means the whole buffer {0, 1}.
     *
     * @param range The pass' band, or nullopt when it states none.
     * @return The (minDepth, maxDepth) pair.
     */
    [[nodiscard]] static constexpr std::pair<float, float> viewportDepth(const std::optional<vn::graphics::DepthRange>& range) noexcept
    {
        if (!range.has_value()) {
            return { 0.0F, 1.0F };
        }
        return { range->min, range->max };
    }
};

VN_BACKEND_VK_NS_END
