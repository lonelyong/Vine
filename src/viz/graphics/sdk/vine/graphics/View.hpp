#pragma once
#include "graphics_global.hpp"

#include <cstdint>

#include <vine/math/Matrix4x4.hpp>

#include "CameraSnapshot.hpp"
#include "Node.hpp"
#include "Viewport.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief ONE VIEW, as a value: the camera it is taken with, the device-pixel area it lands in, and the layers it draws.
 *
 * @details
 * - THE ONE TYPE the design's `View` noun had none of (see `.ai/design/graphics-frame-pipeline.md` §2.2): the derived
 *   layer spoke of a view while the code had two overlapping spellings (`CameraSnapshot` and the retired
 *   `CollectView`) plus a separately passed layer mask. A view is one value, so a frame carries one field and a
 *   collector takes one argument.
 * - WHAT IS SHARED, WHAT DIFFERS: the camera half is a `CameraSnapshot` taken when the pass described itself, so the
 *   culling, the projection and the shading of one frame cannot see a camera that moved in between; the area half is
 *   DEVICE pixels (see Viewport) and is invalid when the caller cannot state one; the mask is the content selection
 *   (see Node::setLayers) and is part of what makes two views draw different lists.
 * - `camera.present == false` is a real answer, not an empty default: the drawing call names no view and issues
 *   nothing (see isDrawingCall). It is never confused with default matrices that happen to be zero.
 *
 * @note
 * - ONE PER VIEW, not one per frame: a frame has shadow, main, PiP and HUD passes with different cameras and areas.
 *   Two passes that state the same camera, area and mask ask for the same list, which is what the collected-list
 *   memo keys on.
 * - The unit of @ref viewport is DEVICE pixels; a host that knows a logical size converts before it states one (see
 *   Viewport). No unit type is introduced here - that distinction has no consumer yet.
 */
struct VN_GRAPHICS_API View {
    /// The camera value this view is taken with; `present == false` means the drawing call issues nothing.
    CameraSnapshot camera{};
    /// The device-pixel rectangle the picture lands in; invalid = the caller could not state one.
    Viewport viewport{};
    /// Layers this picture draws (see Node::setLayers): two passes with different masks do not draw the same list.
    std::uint32_t layer_mask{ kAllLayers };

    /**
     * @brief Gets the projection * view product (the matrix a cull/lod query projects with).
     *
     * @return The combined matrix; meaningless when @ref camera is not present.
     */
    [[nodiscard]] Mat4d viewProjection() const
    {
        return camera.projection * camera.view;
    }

    /**
     * @brief Gets whether this view names a camera at all.
     *
     * @return true when the camera half is engaged (see CameraSnapshot::present).
     */
    [[nodiscard]] bool present() const noexcept
    {
        return camera.present;
    }

    /**
     * @brief Gets whether this view states a device-pixel draw rectangle.
     *
     * @return true when @ref viewport describes a positive area.
     */
    [[nodiscard]] bool hasViewport() const noexcept
    {
        return viewport.isValid();
    }
};

VN_GRAPHICS_NS_END
