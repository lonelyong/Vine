#include <vine/geometry/Mesh.hpp>

#include <cstring>
#include <span>
#include <utility>
#include <vector>

V_GEOMETRY_NS_BEGIN

V_OBJECT_META_IMPL(Mesh, Shape)

// The typed accessors hand out Vec3f / Vec2f views over the SAME scalars the renderer reads, so the two
// layouts must match exactly. Vector3 is a union of `{T x, y, z}` and `T data[3]`, which is what makes the
// reinterpretation sound; these asserts make a future change (padding, reordering, a wider element) fail the
// build instead of silently misreading every vertex.
static_assert(sizeof(vine::math::Vec3f) == Mesh::kVec3Components * sizeof(float));
static_assert(alignof(vine::math::Vec3f) == alignof(float));
static_assert(sizeof(vine::math::Vec2f) == Mesh::kVec2Components * sizeof(float));
static_assert(alignof(vine::math::Vec2f) == alignof(float));

namespace
{

/**
 * @brief Views a run of packed scalars as Vec3 elements.
 *
 * @param scalars Scalars of the attribute (`kVec3Components` per element).
 * @return The same bytes as Vec3f elements.
 */
std::span<const vine::math::Vec3f> asVec3(std::span<const float> scalars)
{
    return { reinterpret_cast<const vine::math::Vec3f*>(scalars.data()),
             scalars.size() / Mesh::kVec3Components };
}

/**
 * @brief Views a run of packed scalars as Vec2 elements.
 *
 * @param scalars Scalars of the attribute (`kVec2Components` per element).
 * @return The same bytes as Vec2f elements.
 */
std::span<const vine::math::Vec2f> asVec2(std::span<const float> scalars)
{
    return { reinterpret_cast<const vine::math::Vec2f*>(scalars.data()),
             scalars.size() / Mesh::kVec2Components };
}

/**
 * @brief Reinterprets a Vec3 array as the scalars the buffer stores.
 *
 * A byte copy, not a conversion — the elements already ARE the scalars. memcpy (rather than casting the
 * pointer) is what keeps it free of aliasing questions.
 *
 * @param elements Elements to reinterpret.
 * @return The same bytes as a scalar array.
 */
std::vector<float> packVec3(Vec3fArray elements)
{
    std::vector<float> scalars(elements.size() * Mesh::kVec3Components);
    if (!scalars.empty()) {
        std::memcpy(scalars.data(), elements.data(), scalars.size() * sizeof(float));
    }
    return scalars;
}

/**
 * @brief Reinterprets a Vec2 array as the scalars the buffer stores.
 *
 * @param elements Elements to reinterpret.
 * @return The same bytes as a scalar array.
 */
std::vector<float> packVec2(Vec2fArray elements)
{
    std::vector<float> scalars(elements.size() * Mesh::kVec2Components);
    if (!scalars.empty()) {
        std::memcpy(scalars.data(), elements.data(), scalars.size() * sizeof(float));
    }
    return scalars;
}

}  // namespace

Mesh::Mesh()
  : positions_(makeBuffer(std::vector<float>{}))
  , normals_(makeBuffer(std::vector<float>{}))
  , texcoords_(makeBuffer(std::vector<float>{}))
{
}

std::span<const vine::math::Vec3f> Mesh::positions() const
{
    return asVec3(positions_->view());
}

std::span<const vine::math::Vec3f> Mesh::normals() const
{
    return asVec3(normals_->view());
}

std::span<const vine::math::Vec2f> Mesh::texcoords() const
{
    return asVec2(texcoords_->view());
}

intrusive_ptr<const Buffer<float>> Mesh::positionsBuffer() const
{
    return positions_;
}

intrusive_ptr<const Buffer<float>> Mesh::normalsBuffer() const
{
    return normals_;
}

intrusive_ptr<const Buffer<float>> Mesh::texcoordsBuffer() const
{
    return texcoords_;
}

void Mesh::setPositions(Vec3fArray positions)
{
    positions_ = makeBuffer(packVec3(std::move(positions)));
}

void Mesh::setNormals(Vec3fArray normals)
{
    normals_ = makeBuffer(packVec3(std::move(normals)));
}

void Mesh::setTexcoords(Vec2fArray texcoords)
{
    texcoords_ = makeBuffer(packVec2(std::move(texcoords)));
}

std::size_t Mesh::vertexCount() const
{
    return positions_->size() / kVec3Components;
}

const Aabbf& Mesh::aabb() const
{
    return aabb_;
}

void Mesh::setAabb(const Aabbf& aabb)
{
    aabb_ = aabb;
}

Aabbf Mesh::computeAabb()
{
    aabb_ = Aabbf::compute(positions());
    return aabb_;
}

void Mesh::clearAttributes()
{
    positions_->clear();
    normals_->clear();
    texcoords_->clear();
    aabb_ = Aabbf::empty();
}

V_GEOMETRY_NS_END
