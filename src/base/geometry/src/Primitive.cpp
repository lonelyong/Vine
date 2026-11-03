#include <vine/geometry/Primitive.hpp>

// The five primitives define every member inline, so no translation unit of this DLL would otherwise instantiate them.
// Listing them here is what makes the geometry DLL emit and export their inline members: a consumer that constructs a
// Box/Sphere/... in a build that does not inline (/Ob0, i.e. Debug) would otherwise reference an import symbol that
// nothing exports and fail to link.
#include <vine/geometry/Box.hpp>
#include <vine/geometry/Cone.hpp>
#include <vine/geometry/Cylinder.hpp>
#include <vine/geometry/Ellipsoid.hpp>
#include <vine/geometry/Sphere.hpp>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(Primitive, Shape)

VN_OBJECT_META_IMPL(Box, Primitive)

VN_OBJECT_META_IMPL(Cone, Primitive)

VN_OBJECT_META_IMPL(Cylinder, Primitive)

VN_OBJECT_META_IMPL(Ellipsoid, Primitive)

VN_OBJECT_META_IMPL(Sphere, Primitive)

Primitive::Primitive()
{}

VN_GEOMETRY_NS_END