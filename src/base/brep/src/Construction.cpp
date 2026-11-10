#include <vine/brep/Construction.hpp>

#if defined(VN_BREP_HAS_OCCT)
#    include <exception>
#    include <memory>
#    include <utility>

#    include <BRepBuilderAPI_GTransform.hxx>
#    include <BRepPrimAPI_MakeBox.hxx>
#    include <BRepPrimAPI_MakeCone.hxx>
#    include <BRepPrimAPI_MakeCylinder.hxx>
#    include <BRepPrimAPI_MakeSphere.hxx>
#    include <Standard_Failure.hxx>
#    include <TopoDS_Shape.hxx>
#    include <gp_Ax2.hxx>
#    include <gp_Dir.hxx>
#    include <gp_GTrsf.hxx>
#    include <gp_Mat.hxx>
#    include <gp_Pnt.hxx>

#    include <vine/Object.hpp>
#    include <vine/geom/spatial/BoxSolid.hpp>
#    include <vine/geom/spatial/ConeSolid.hpp>
#    include <vine/geom/spatial/CylinderSolid.hpp>
#    include <vine/geom/spatial/EllipsoidSolid.hpp>
#    include <vine/geom/spatial/SphereSolid.hpp>
#    include <vine/models/Primitive.hpp>
#    include <vine/models/PrimitiveDispatch.hpp>
#endif

VN_BREP_NS_BEGIN

#if defined(VN_BREP_HAS_OCCT)

namespace
{

/// The axis cylinder and cone are built around.
///
/// The backend grows these solids from the axis origin along +Z, while the primitives are centered on the origin, so
/// the axis starts at the -height/2 face.
gp_Ax2 centeredZAxis(double half_height)
{
    return gp_Ax2(gp_Pnt(0.0, 0.0, -half_height), gp_Dir(0.0, 0.0, 1.0));
}

/// Builds a box spanning [-hx, hx] x [-hy, hy] x [-hz, hz], the space the analytic box describes.
TopoDS_Shape solidOf(const vn::geom::spatial::BoxSolidf& box)
{
    const auto half_x = static_cast<double>(box.half_x);
    const auto half_y = static_cast<double>(box.half_y);
    const auto half_z = static_cast<double>(box.half_z);

    const gp_Pnt corner(-half_x, -half_y, -half_z);
    return BRepPrimAPI_MakeBox(corner, half_x * 2.0, half_y * 2.0, half_z * 2.0).Shape();
}

/// Builds a sphere of the given radius, already centered on the origin.
TopoDS_Shape solidOf(const vn::geom::spatial::SphereSolidf& sphere)
{
    return BRepPrimAPI_MakeSphere(static_cast<double>(sphere.radius)).Shape();
}

/// Builds a cylinder spanning [-h/2, h/2] on +Z, the space the analytic cylinder describes.
TopoDS_Shape solidOf(const vn::geom::spatial::CylinderSolidf& cylinder)
{
    const auto radius = static_cast<double>(cylinder.radius);
    const auto height = static_cast<double>(cylinder.half_height) * 2.0;

    return BRepPrimAPI_MakeCylinder(centeredZAxis(height / 2.0), radius, height).Shape();
}

/// Builds a cone with its base at -h/2 and its apex at +h/2 on +Z, the space the analytic cone describes.
TopoDS_Shape solidOf(const vn::geom::spatial::ConeSolidf& cone)
{
    const auto radius = static_cast<double>(cone.base_radius);
    const auto height = static_cast<double>(cone.half_height) * 2.0;

    return BRepPrimAPI_MakeCone(centeredZAxis(height / 2.0), radius, 0.0, height).Shape();
}

/// Builds an axis-aligned ellipsoid centered on the origin.
///
/// The backend has no tri-axial ellipsoid: a unit sphere is scaled onto the three semi-axes, which turns its face into
/// a B-spline, so the solid matches the ellipsoid only within the transform's tolerance.
TopoDS_Shape solidOf(const vn::geom::spatial::EllipsoidSolidf& ellipsoid)
{
    const auto radius_x = static_cast<double>(ellipsoid.radius_x);
    const auto radius_y = static_cast<double>(ellipsoid.radius_y);
    const auto radius_z = static_cast<double>(ellipsoid.radius_z);

    gp_GTrsf scale;
    scale.SetVectorialPart(gp_Mat(radius_x, 0.0, 0.0, 0.0, radius_y, 0.0, 0.0, 0.0, radius_z));

    const TopoDS_Shape unit_sphere = BRepPrimAPI_MakeSphere(1.0).Shape();
    // `Shape()` is a non-const member in OpenCASCADE 8, so the builder cannot be const-qualified here.
    BRepBuilderAPI_GTransform scaled(unit_sphere, scale, Standard_True);
    return scaled.Shape();
}

/// Builds the solid the primitive describes.
TopoDS_Shape buildSolid(const vn::models::Primitive& primitive)
{
    // The dispatch visits the closed set of modelling primitives: each subclass hands over its own analytic solid, and a
    // primitive the bridge does not know answers an empty shape rather than being silently skipped.
    TopoDS_Shape built;
    vn::models::visitPrimitive(primitive, [&](const auto& shaped) { built = solidOf(shaped.solid()); });
    return built;
}

} // namespace

#endif

vn::intrusive_ptr<vn::models::BrepShape> makeBrep(const vn::models::Primitive& primitive)
{
    if (!primitive.isValid()) {
        return {}; // an invalid primitive is an input-level cause, not a build-level one
    }

#if defined(VN_BREP_HAS_OCCT)
    try {
        TopoDS_Shape solid = buildSolid(primitive);
        if (solid.IsNull()) {
            return {};
        }

        auto result = vn::make_intrusive<vn::models::BrepShape>();
        result->setShape(std::make_shared<const TopoDS_Shape>(std::move(solid)));
        result->setName(primitive.name());
        return result;
    }
    catch (const Standard_Failure&) {
        return {}; // a backend failure is "no solid", the same answer as no backend
    }
    catch (const std::exception&) {
        return {};
    }
#else
    // Nothing is produced without the backend. hasBackend() tells a build-level cause from an input-level one.
    return {};
#endif
}

VN_BREP_NS_END
