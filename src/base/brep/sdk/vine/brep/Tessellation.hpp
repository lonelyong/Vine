#pragma once

#include "brep_global.hpp"

#include <vine/geometry/BrepShape.hpp>
#include <vine/geometry/TriangleMesh.hpp>
#include <vine/intrusive_ptr.hpp>

#include "Backend.hpp"

VN_BREP_NS_BEGIN

/**
 * @brief Options that trade triangle count against how closely the mesh follows a curved face.
 */
struct TessellationOptions {
    /// Maximum distance between a face and the triangles that stand in for it, in the solid's units.
    double deflection{ 0.1 };

    /// Maximum angle between the normals of two adjacent triangles inside one curved face, in radians.
    double angle{ 0.5 };

    /// Whether a normal is produced for every vertex; false keeps positions and indices only.
    bool compute_normals{ true };
};

/**
 * @brief Triangulates a B-rep solid into a triangle mesh.
 *
 * @details
 * - The mesh stays in the solid's own space: nothing is placed, scaled or transformed here, so a caller applies
 *   its own transform to the result.
 * - Vertices are per face, not welded across a seam: two faces meeting at an edge keep one vertex each, because
 *   their normals differ there. The index stream is wound counter-clockwise as the owning face sees it.
 * - The result carries no bounds: a caller that needs them measures the returned positions with `Aabb::compute()`.
 * - Failure is a null mesh, and it has exactly two causes: this build has no backend (ask hasBackend() when the
 *   reason matters) or the solid holds no shape.
 *
 * @note
 * - The OpenCASCADE body is compiled only when the backend is found (`VN_BREP_HAS_OCCT`); a build without it
 *   returns a null mesh and reports false from hasBackend(). That branch still needs a build with the backend to be
 *   verified (see `.ai/backlog/brep.md`).
 *
 * @param shape   The solid to triangulate.
 * @param options Deflection, angular tolerance and normal production.
 * @return The triangle mesh, or null when nothing was produced.
 */
[[nodiscard]] VN_BREP_API vn::intrusive_ptr<vn::geometry::TriangleMesh>
tessellate(const vn::geometry::BrepShape& shape, const TessellationOptions& options = {});

VN_BREP_NS_END
