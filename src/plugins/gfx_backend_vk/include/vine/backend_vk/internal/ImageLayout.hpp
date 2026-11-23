#pragma once

#include <cstdint>

#include <vine/backend_vk/backend_vk_global.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief What an image is being USED for right now - this backend's input to the barrier table.
 *
 * @details
 * - WHY IT LIVES HERE AND NOT IN THE SDK. A layout, a stage mask and an access mask are one family's spelling
 *   of "this image is being written as colour"; D3D12 states a resource state and Metal tracks hazards itself,
 *   so the SDK states only what every family can answer (`LoadOp` / `StoreOp`, and what a pass declares) and
 *   each backend derives its own transitions. This enum was the last Vulkan-flavoured type on the public
 *   surface (backlog T15); review 8 §4.7/§4.8 records the move.
 * - THE FIVE VALUES ARE THE PASS STRUCTURES THIS BACKEND PRODUCES, not Vulkan's full layout list: the two
 *   transfer purposes are this module's own (`transferReadState()`), because a readback or an upload is a
 *   DEVICE verb rather than something a pass declares.
 */
enum class ImageLayout : std::uint8_t
{
    Undefined,       // No layout; an image being used for the first time.
    ColorAttachment, // Being written as a colour attachment.
    DepthAttachment, // Being written as a depth attachment.
    ShaderReadOnly,  // Being sampled.
    Present,         // Handed to the presentation engine.
};

VN_BACKEND_VK_NS_END
