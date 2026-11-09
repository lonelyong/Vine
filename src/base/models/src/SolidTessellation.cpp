#include <vine/models/SolidTessellation.hpp>

#include <utility>

VN_MODELS_NS_BEGIN

vn::intrusive_ptr<TriangleMesh> meshFromTriangulation(vn::geom::spatial::Triangulationf triangulation)
{
    if (triangulation.empty()) {
        return {};
    }

    auto mesh = vn::make_intrusive<TriangleMesh>();
    mesh->setPositions(std::move(triangulation.positions));
    mesh->setNormals(std::move(triangulation.normals));
    mesh->setIndices(std::move(triangulation.indices));
    (void)mesh->computeAabb();
    return mesh;
}

VN_MODELS_NS_END
