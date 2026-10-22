#pragma once
#include "graphics_global.hpp"

#include <cstdint>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief How a primitive's vertices assemble into primitives.
 *
 * @details
 * - A PRIMITIVE's fact, not a subtree state: it says how this draw consumes its vertex/index data, which
 *   is why it lives on `Primitive` (see Primitive.hpp) rather than on a `StateNode`. Two draws over one
 *   vertex set may assemble differently (a triangle list and its edge line list), and only the data can
 *   answer which is which.
 * - It is IDENTITY for a pipeline (see core::PipelineKey::topology) and not merely dynamic state: the API
 *   lets a dynamic topology move only within the pipeline's own topology CLASS, so a point cloud drawn
 *   through a triangle-baked pipeline is undefined behaviour rather than a picture.
 * - What it is NOT: `PolygonMode` (fill / line / point) is how assembled triangles RASTERISE - one mesh
 *   draws filled, as wireframe or as points without touching its data or its topology.
 *
 * @note A pass may OVERRIDE it for a whole view ("show me this scene as points"); that is a viewing
 *       decision and lives on the pass (see RenderPass::stateOverrides), the same way it already overrides
 *       the program. The data keeps saying what it is.
 *
 * @note @ref Topology::Points is a contract on the PROGRAM as well as on the data: a POINT_LIST pipeline
 *       needs a vertex stage that statically writes `gl_PointSize`, and without it (and without
 *       `maintenance5`) the pipeline and every draw with it are invalid. The built-in programs write 1.0,
 *       so any geometry can be drawn as points out of the box; a caller that wants points bigger than one
 *       pixel supplies a program that writes the size it wants.
 */
enum class Topology
{
    Triangles,  ///< Triangle list (the default).
    Points,     ///< Unconnected points (e.g. point clouds).
    Lines,      ///< Line list.
};

VN_GRAPHICS_NS_END
