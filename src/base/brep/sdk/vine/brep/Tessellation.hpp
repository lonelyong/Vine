#pragma once

#include "brep_global.hpp"

#include <vine/geometry/BrepShape.hpp>
#include <vine/geometry/TriangleMesh.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/io/IoError.hpp>

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
 * - Without the backend this answers `IoError::Unsupported` — the same failure a build without the backend
 *   reports elsewhere — and a solid that holds no shape answers `IoError::InvalidData`.
 *
 * @note
 * - The OpenCASCADE body is backlog work (see `.ai/backlog/brep.md`); this module carries the contract, the
 *   backend branch is added behind `VN_BREP_HAS_OCCT` when a build with the backend exists.
 *
 * @param shape   The solid to triangulate.
 * @param options Deflection, angular tolerance and normal production.
 * @return The triangle mesh, or the failure class.
 */
[[nodiscard]] VN_BREP_API vn::io::Result<vn::intrusive_ptr<vn::geometry::TriangleMesh>>
tessellate(const vn::geometry::BrepShape& shape, const TessellationOptions& options = {});

VN_BREP_NS_END
