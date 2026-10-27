#include <vine/geometry/BrepShape.hpp>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(BrepShape, Shape)

BrepShape::BrepShape()
{
    shape_type_ = ShapeType::Brep;
}

BrepShape::~BrepShape() = default;

const TopoDS_Shape* BrepShape::shape() const noexcept
{
    return shape_.get();
}

void BrepShape::setShape(std::shared_ptr<const TopoDS_Shape> shape)
{
    shape_ = std::move(shape);
}

bool BrepShape::isValid() const
{
    return shape_ != nullptr;
}

VN_GEOMETRY_NS_END
