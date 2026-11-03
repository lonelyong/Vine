#include <vine/geometry/PointCloud.hpp>

#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(PointCloud, Shape)

namespace {

/// Scalars per position and per normal, the layout `Buffer<float>` stores.
constexpr std::size_t kVec3Components = 3u;

/// Scalars per linear RGBA colour.
constexpr std::size_t kVec4Components = 4u;

/**
 * @brief Reinterprets packed scalars as elements.
 *
 * @tparam Components Scalars per element.
 * @tparam V          Element type, laid out as those scalars are.
 *
 * @param scalars The packed scalars.
 * @return View over the same bytes, typed as elements.
 */
template <std::size_t Components, typename V>
std::span<const V> asTyped(std::span<const float> scalars)
{
    static_assert(std::is_trivially_copyable_v<V>);
    static_assert(sizeof(V) == Components * sizeof(float));
    static_assert(alignof(V) == alignof(float));

    return { reinterpret_cast<const V*>(scalars.data()), scalars.size() / Components };
}

/**
 * @brief Packs elements into their scalar run by copying their bytes.
 *
 * @tparam Components Scalars per element.
 * @tparam V          Element type, laid out as those scalars are.
 *
 * @param values The elements to pack.
 * @return The packed scalars, Components per element.
 */
template <std::size_t Components, typename V>
std::vector<float> packTyped(const std::vector<V>& values)
{
    static_assert(std::is_trivially_copyable_v<V>);
    static_assert(sizeof(V) == Components * sizeof(float));

    std::vector<float> scalars(values.size() * Components);
    if (!scalars.empty()) {
        std::memcpy(scalars.data(), values.data(), scalars.size() * sizeof(float));
    }
    return scalars;
}

} // namespace

PointCloud::PointCloud()
  : positions_(vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>()))
  , normals_(vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>()))
  , colors_(vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>()))
  , aabb_(Aabb::empty())
{
    shape_type_ = ShapeType::PointCloud;
}

PointCloud::~PointCloud() = default;

std::span<const PointCloud::Vec3> PointCloud::positions() const
{
    return asTyped<kVec3Components, Vec3>(positions_->view());
}

vn::intrusive_ptr<const vn::Buffer<float>> PointCloud::positionsBuffer() const
{
    return positions_;
}

std::span<const PointCloud::Vec3> PointCloud::normals() const
{
    return asTyped<kVec3Components, Vec3>(normals_->view());
}

vn::intrusive_ptr<const vn::Buffer<float>> PointCloud::normalsBuffer() const
{
    return normals_;
}

std::span<const PointCloud::Vec4> PointCloud::colors() const
{
    return asTyped<kVec4Components, Vec4>(colors_->view());
}

vn::intrusive_ptr<const vn::Buffer<float>> PointCloud::colorsBuffer() const
{
    return colors_;
}

void PointCloud::setPositions(Vec3Array positions)
{
    positions_ = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(packTyped<kVec3Components>(positions)));
    aabb_ = Aabb::empty();
}

void PointCloud::setNormals(Vec3Array normals)
{
    normals_ = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(packTyped<kVec3Components>(normals)));
}

void PointCloud::setColors(Vec4Array colors)
{
    colors_ = vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(packTyped<kVec4Components>(colors)));
}

std::size_t PointCloud::pointCount() const
{
    return positions_->size() / kVec3Components;
}

const PointCloud::Aabb& PointCloud::aabb() const
{
    return aabb_;
}

void PointCloud::setAabb(const Aabb& aabb)
{
    aabb_ = aabb;
}

PointCloud::Aabb PointCloud::computeAabb()
{
    aabb_ = Aabb::compute(positions());
    return aabb_;
}

bool PointCloud::isValid() const
{
    return positions_->size() != 0u;
}

VN_GEOMETRY_NS_END
