#pragma once
#include "graphics_global.hpp"

#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

#include "Viewport.hpp"

VN_GRAPHICS_NS_BEGIN

using vn::math::Mat4d;
using vn::math::Point3d;
using vn::math::Vec3d;

/**
 * @brief The view one command list is collected for.
 *
 * @details
 * - The collection-time context: the camera's projection / view / eye / up plus the DEVICE-pixel
 *   viewport the picture lands in. It is the narrow counterpart of OSG's `CullStack` - the facts a
 *   view-dependent node needs, without any blend / depth / cull state (that state is resolved per
 *   command by the pass, see StateNode.hpp).
 * - ONE PER VIEW, not one per frame: a frame has shadow, main, PiP and HUD passes with different
 *   cameras and viewports. The scene's collected-list memo is keyed by the view-projection and eye,
 *   plus the viewport only when the list contains a node that depends on it (ViewDependence::
 *   ViewAndViewport) - a plain scene drawn into two rectangles still shares one list.
 * - @ref viewport is DEVICE pixels (see Viewport). It is invalid when the caller cannot state one
 *   (a host collecting without a target); view-dependent features then keep their authored values.
 *
 * @note
 * - This is deliberately NOT a frame context. Frame index / time belong to the update or pass level
 *   (OSG: NodeVisitor + FrameStamp); putting them here would make the scene memo stale every frame.
 */
struct VN_GRAPHICS_API CollectView {
    Mat4d    projection{};            ///< Camera projection matrix.
    Mat4d    view{};                  ///< Camera world-to-view matrix.
    Vec3d    eye{};                   ///< Camera eye point in world space.
    Vec3d    up{ 0.0, 1.0, 0.0 };     ///< Camera up direction in world space.
    Viewport viewport{};              ///< Device-pixel draw rectangle; invalid = not stated.

    /** @brief Gets the projection * view product (the matrix a cull/lod query projects with). */
    [[nodiscard]] Mat4d viewProjection() const
    {
        return projection * view;
    }

    /** @brief Gets whether this view states a device-pixel draw rectangle. */
    [[nodiscard]] bool hasViewport() const noexcept
    {
        return viewport.isValid();
    }
};

VN_GRAPHICS_NS_END
