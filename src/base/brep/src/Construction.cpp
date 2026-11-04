#include <vine/brep/Construction.hpp>

#if defined(VN_BREP_HAS_OCCT)
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

#    include <exception>
#    include <memory>
#    include <utility>

#    include <vine/Object.hpp>
#    include <vine/geometry/Primitive.hpp>
#endif

VN_BREP_NS_BEGIN

#if defined(VN_BREP_HAS_OCCT)

namespace {

/// The axis cylinder and cone are built around.
///
/// The backend grows these solids from the axis origin along +Z, while the primitives are centered on the origin, so
/// the axis starts at the -height/2 face.
gp_Ax2 centeredZAxis(double half_height)
{
    return gp_Ax2(gp_Pnt(0.0, 0.0, -half_height), gp_Dir(0.0, 0.0, 1.0));
}

/// Builds a box spanning [-w/2, w/2] x [-h/2, h/2] x [-d/2, d/2], the space Box describes.
TopoDS_Shape solidOf(const vn::geometry::Box& box)
{
    const auto width  = static_cast<double>(box.width());
    const auto height = static_cast<double>(box.height());
    const auto depth  = static_cast<double>(box.depth());

    const gp_Pnt corner(-width / 2.0, -height / 2.0, -depth / 2.0);
    return BRepPrimAPI_MakeBox(corner, width, height, depth).Shape();
}

/// Builds a sphere of the given radius, already centered on the origin.
TopoDS_Shape solidOf(const vn::geometry::Sphere& sphere)
{
    return BRepPrimAPI_MakeSphere(static_cast<double>(sphere.radius())).Shape();
}

/// Builds a cylinder spanning [-h/2, h/2] on +Z, the space Cylinder describes.
TopoDS_Shape solidOf(const vn::geometry::Cylinder& cylinder)
{
    const auto radius = static_cast<double>(cylinder.radius());
    const auto height = static_cast<double>(cylinder.height());

    return BRepPrimAPI_MakeCylinder(centeredZAxis(height / 2.0), radius, height).Shape();
}

/// Builds a cone with its base at -h/2 and its apex at +h/2 on +Z, the space Cone describes.
TopoDS_Shape solidOf(const vn::geometry::Cone& cone)
{
    const auto radius = static_cast<double>(cone.radius());
    const auto height = static_cast<double>(cone.height());

    return BRepPrimAPI_MakeCone(centeredZAxis(height / 2.0), radius, 0.0, height).Shape();
}

/// Builds an axis-aligned ellipsoid centered on the origin.
///
/// The backend has no tri-axial ellipsoid: a unit sphere is scaled onto the three semi-axes, which turns its face into
/// a B-spline, so the solid matches the ellipsoid only within the transform's tolerance.
TopoDS_Shape solidOf(const vn::geometry::Ellipsoid& ellipsoid)
{
    const auto radius_x = static_cast<double>(ellipsoid.radiusX());
    const auto radius_y = static_cast<double>(ellipsoid.radiusY());
    const auto radius_z = static_cast<double>(ellipsoid.radiusZ());

    gp_GTrsf scale;
    scale.SetVectorialPart(gp_Mat(radius_x, 0.0, 0.0, 0.0, radius_y, 0.0, 0.0, 0.0, radius_z));

    const TopoDS_Shape unit_sphere = BRepPrimAPI_MakeSphere(1.0).Shape();
    const BRepBuilderAPI_GTransform scaled(unit_sphere, scale, Standard_True);
    return scaled.Shape();
}

/// Builds the solid the primitive describes, or a null shape when its type is not one the geometry module defines.
TopoDS_Shape buildSolid(const vn::geometry::Primitive& primitive)
{
    // The cast is checked as well as the type: a subclass is free to report a ShapeType it does not implement.
    switch (primitive.shapeType()) {
    case vn::geometry::ShapeType::Box:
        if (const auto* box = vn::obj_cast<vn::geometry::Box>(&primitive)) {
            return solidOf(*box);
        }
        break;
    case vn::geometry::ShapeType::Sphere:
        if (const auto* sphere = vn::obj_cast<vn::geometry::Sphere>(&primitive)) {
            return solidOf(*sphere);
        }
        break;
    case vn::geometry::ShapeType::Cylinder:
        if (const auto* cylinder = vn::obj_cast<vn::geometry::Cylinder>(&primitive)) {
            return solidOf(*cylinder);
        }
        break;
    case vn::geometry::ShapeType::Cone:
        if (const auto* cone = vn::obj_cast<vn::geometry::Cone>(&primitive)) {
            return solidOf(*cone);
        }
        break;
    case vn::geometry::ShapeType::Ellipsoid:
        if (const auto* ellipsoid = vn::obj_cast<vn::geometry::Ellipsoid>(&primitive)) {
            return solidOf(*ellipsoid);
        }
        break;
    default: break;
    }

    return {};
}

} // namespace

#endif

vn::intrusive_ptr<vn::geometry::BrepShape> makeBrep(const vn::geometry::Primitive& primitive)
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

        auto result = vn::make_intrusive<vn::geometry::BrepShape>();
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
