#pragma once
#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief How triangles are rasterised: filled, as their edges, or as their vertices.
 *
 * The one rasterisation choice a viewer makes in its menus - "shaded", "wireframe", "points" - and it is
 * a RASTERISER state rather than a shading one: the same geometry, the same program and the same lights
 * produce outlines instead of surfaces. That is why it is not a property of the content (a StateNode may
 * set it for a subtree, see StateNode::setPolygonMode) and not a shading mode: a pass can override it for
 * everything it draws (RenderPass::setPolygonMode), which is what a view's "wireframe" switch uses.
 *
 * ADDING AN ENUMERATOR IS A BACKEND QUESTION, NOT ONLY AN SDK ONE: what a mode means to a device
 * (VK_POLYGON_MODE_FILL / _LINE / _POINT) is derived in the vsg backend
 * (detail::mapPolygonMode, plus the dynamic-state command that announces it), and a device that cannot
 * draw lines reports it rather than serving a filled picture.
 */
enum class PolygonMode
{
    Fill,  ///< Filled polygons (the default).
    Line,  ///< Wireframe outlines.
    Point, ///< Points at the polygon vertices.
};

VN_GRAPHICS_NS_END
