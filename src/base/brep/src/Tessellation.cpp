#include <vine/brep/Tessellation.hpp>

#if defined(VN_BREP_HAS_OCCT)
#    include <BRepMesh_IncrementalMesh.hxx>
#    include <BRep_Tool.hxx>
#    include <GeomLib.hxx>
#    include <Geom_Surface.hxx>
#    include <IMeshTools_Parameters.hxx>
#    include <NCollection_IndexedMap.hxx>
#    include <Poly_Connect.hxx>
#    include <Poly_Triangulation.hxx>
#    include <Poly_Triangle.hxx>
#    include <Precision.hxx>
#    include <Standard_Failure.hxx>
#    include <TopAbs_Orientation.hxx>
#    include <TopExp.hxx>
#    include <TopLoc_Location.hxx>
#    include <TopTools_ShapeMapHasher.hxx>
#    include <TopoDS.hxx>
#    include <TopoDS_Face.hxx>
#    include <TopoDS_Shape.hxx>
#    include <gp_Dir.hxx>
#    include <gp_Pnt.hxx>
#    include <gp_Trsf.hxx>
#    include <gp_XYZ.hxx>

#    include <cstdint>
#    include <exception>
#    include <utility>
#endif

VN_BREP_NS_BEGIN

bool tessellationSupported() noexcept
{
#if defined(VN_BREP_HAS_OCCT)
    return true;
#else
    return false;
#endif
}

#if defined(VN_BREP_HAS_OCCT)

namespace {

/**
 * @brief Appends one normal per node of @p triangulation, in the solid's own space.
 *
 * @details
 * - Normals stored in the triangulation are used as they are; otherwise the surface normal at the node's UV
 *   coordinate is estimated, and a face without UV nodes falls back to the area-weighted average of its incident
 *   triangles.
 * - A reversed face flips the block it appended, because its triangulation carries the surface's own orientation.
 *
 * @param face          The face the triangulation belongs to.
 * @param triangulation The face's triangulation, one node per entry of the block.
 * @param location      Placement of the triangulation inside the solid.
 * @param normals       Output array; the face's block is appended at its end.
 */
void appendFaceNormals(const TopoDS_Face&                     face,
                       const Handle(Poly_Triangulation) &      triangulation,
                       const TopLoc_Location&                 location,
                       vn::geometry::Vec3Array&               normals)
{
    const std::size_t block_begin = normals.size();
    const int         node_count  = triangulation->NbNodes();

    const auto append_direction = [&](const gp_Dir& direction) {
        gp_Dir placed = direction;
        placed.Transform(location.Transformation());
        normals.emplace_back(static_cast<float>(placed.X()), static_cast<float>(placed.Y()), static_cast<float>(placed.Z()));
    };

    if (triangulation->HasNormals()) {
        for (int node = 1; node <= node_count; ++node) {
            append_direction(triangulation->Normal(node));
        }
    }
    else {
        Poly_Connect connect(triangulation);

        // The face's location is cleared first: BRep_Tool::Surface wants the surface the face lies on, and the
        // placement is applied to the normal through @p location afterwards.
        const Handle(Geom_Surface) surface = BRep_Tool::Surface(TopoDS::Face(face.Located(TopLoc_Location{})));

        for (int node = 1; node <= node_count; ++node) {
            gp_Dir estimated;

            if (!surface.IsNull() && triangulation->HasUVNodes() &&
                GeomLib::NormEstim(surface, triangulation->UVNode(node), Precision::Confusion(), estimated)) {
                append_direction(estimated);
                continue;
            }

            // No surface estimate: average the unit normals of the triangles around this node, so a large triangle
            // does not outweigh a small one.
            gp_XYZ accumulated(0.0, 0.0, 0.0);
            for (connect.Initialize(node); connect.More(); connect.Next()) {
                Standard_Integer first  = 0;
                Standard_Integer second = 0;
                Standard_Integer third  = 0;
                triangulation->Triangle(connect.Value()).Get(first, second, third);

                const gp_XYZ edge_a = triangulation->Node(second).Coord() - triangulation->Node(first).Coord();
                const gp_XYZ edge_b = triangulation->Node(third).Coord() - triangulation->Node(second).Coord();
                const gp_XYZ cross  = edge_a ^ edge_b;

                const double length = cross.Modulus();
                if (length > Precision::Confusion()) {
                    accumulated += cross / length;
                }
            }

            if (accumulated.Modulus() > Precision::Confusion()) {
                estimated.SetCoord(accumulated.X(), accumulated.Y(), accumulated.Z());
                append_direction(estimated);
            }
            else {
                // A degenerate corner still needs a unit normal: +Z is as good as any and keeps the array in step
                // with the positions.
                append_direction(gp_Dir(0.0, 0.0, 1.0));
            }
        }
    }

    if (face.Orientation() == TopAbs_REVERSED) {
        for (std::size_t index = block_begin; index < normals.size(); ++index) {
            normals[index] = -normals[index];
        }
    }
}

} // namespace

#endif // defined(VN_BREP_HAS_OCCT)

vn::intrusive_ptr<vn::geometry::TriangleMesh>
tessellate(const vn::geometry::BrepShape& shape, const TessellationOptions& options)
{
#if defined(VN_BREP_HAS_OCCT)
    const TopoDS_Shape* const solid = shape.shape();
    if (solid == nullptr || solid->IsNull()) {
        return {};
    }

    try {
        IMeshTools_Parameters parameters;
        parameters.Deflection           = options.deflection;
        parameters.Angle                = options.angle;
        parameters.InParallel           = true;
        parameters.Relative             = false; // the option is an absolute length, not a fraction of the face
        parameters.InternalVerticesMode = false;
        parameters.CleanModel           = true;

        BRepMesh_IncrementalMesh mesher(*solid, parameters);
        mesher.Perform();

        NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> faces;
        TopExp::MapShapes(*solid, TopAbs_FACE, faces);

        vn::geometry::Vec3Array   positions;
        vn::geometry::UInt32Array indices;
        vn::geometry::Vec3Array   normals;

        for (int face_index = 1; face_index <= faces.Extent(); ++face_index) {
            const TopoDS_Face face = TopoDS::Face(faces.FindKey(face_index));
            TopLoc_Location   location;

            const Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, location);
            if (triangulation.IsNull()) {
                continue; // a face the mesher could not triangulate contributes nothing
            }

            const auto base = static_cast<std::uint32_t>(positions.size());

            for (int node = 1; node <= triangulation->NbNodes(); ++node) {
                gp_Pnt vertex = triangulation->Node(node);
                vertex.Transform(location.Transformation());
                positions.emplace_back(static_cast<float>(vertex.X()), static_cast<float>(vertex.Y()), static_cast<float>(vertex.Z()));
            }

            for (int triangle_index = 1; triangle_index <= triangulation->NbTriangles(); ++triangle_index) {
                Standard_Integer first  = 0;
                Standard_Integer second = 0;
                Standard_Integer third  = 0;
                triangulation->Triangle(triangle_index).Get(first, second, third);

                if (face.Orientation() == TopAbs_REVERSED) {
                    std::swap(first, second); // the winding follows the face, not the surface
                }

                indices.push_back(base + static_cast<std::uint32_t>(first - 1));
                indices.push_back(base + static_cast<std::uint32_t>(second - 1));
                indices.push_back(base + static_cast<std::uint32_t>(third - 1));
            }

            if (options.compute_normals) {
                appendFaceNormals(face, triangulation, location, normals);
            }
        }

        if (indices.empty()) {
            return {};
        }

        auto mesh = vn::make_intrusive<vn::geometry::TriangleMesh>();
        mesh->setPositions(std::move(positions));
        mesh->setIndices(std::move(indices));
        if (options.compute_normals) {
            mesh->setNormals(std::move(normals));
        }
        return mesh;
    }
    catch (const Standard_Failure&) {
        return {}; // a backend failure is "no mesh", the same answer as no backend
    }
    catch (const std::exception&) {
        return {};
    }
#else
    (void)shape;
    (void)options;

    // Nothing is produced without the backend. The contract in the header names the two causes of a null mesh;
    // tessellationSupported() tells a build-level cause from an input-level one.
    return {};
#endif
}

VN_BREP_NS_END
