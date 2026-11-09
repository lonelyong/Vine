#pragma once

#include <vine/models/models_global.hpp>

#include <vine/geom/spatial/SolidTessellation.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/models/TriangleMesh.hpp>

VN_MODELS_NS_BEGIN

/**
 * @brief Builds the model mesh a tessellated solid describes, taking its arrays over.
 *
 * @details
 * - The geometry side answers in three arrays (`vn::geom::spatial::Triangulationf`); a mesh is a model, so the step that
 *   puts those triangles into one belongs here and nowhere below.
 * - The arrays are MOVED into the mesh's channels: building the mesh costs no copy of a vertex.
 * - The cached box is computed before the mesh is handed out, because the positions are already in memory.
 * - The parameter is the `float` triangulation on purpose: a model mesh is float, so a `Triangulationd` is converted
 *   explicitly by the caller instead of being narrowed here without saying so.
 *
 * @param triangulation The triangles to adopt; an empty value answers null.
 * @return The mesh, or null when @p triangulation is empty.
 */
[[nodiscard]] VN_MODELS_API vn::intrusive_ptr<TriangleMesh> meshFromTriangulation(vn::geom::spatial::Triangulationf triangulation);

VN_MODELS_NS_END
