#include <vine/meshio/BinStlSource.hpp>

#include <cstring>
#include <utility>

VN_MESHIO_NS_BEGIN

namespace
{

using Mesh = vn::geometry::Mesh;

/// Bytes a binary STL reserves for its free-text header.
constexpr std::size_t kHeaderSize = 80;
/// Triangles a binary STL can count: the field is 32 bits wide.
constexpr std::uint64_t kMaxTriangles = 0xFFFFFFFFull;

/**
 * @brief Writes one float into a byte range, in the host's own order.
 *
 * Binary STL is a raw layout, like every other raw layout in this library: it is written in host order, so the same
 * machine reads back what it wrote.
 *
 * @param out The record being filled.
 * @param offset Byte offset to write at.
 * @param value The value to write.
 */
void putFloat(char* out, std::size_t offset, float value)
{
    std::memcpy(out + offset, &value, sizeof(float));
}

/**
 * @brief Writes three floats into a byte range.
 *
 * @param out The record being filled.
 * @param offset Byte offset to write at.
 * @param values The three values to write.
 */
void putVec3(char* out, std::size_t offset, const float (&values)[3])
{
    putFloat(out, offset + 0, values[0]);
    putFloat(out, offset + 4, values[1]);
    putFloat(out, offset + 8, values[2]);
}

} // namespace

BinStlSource::BinStlSource(vn::intrusive_ptr<const Mesh> mesh, double scale) : MeshSource(std::move(mesh), scale)
{
    if (!ready()) {
        return;
    }

    // The triangle count goes into a 32-bit field, so this is the one thing binary STL cannot write.
    if (triangleCount() > kMaxTriangles) {
        refuse();
        return;
    }
    stateSize(kPrefixSize + kTriangleSize * triangleCount());
}

BinStlSource::~BinStlSource() = default;

BinStlSource::BinStlSource(BinStlSource&& other) noexcept = default;

BinStlSource& BinStlSource::operator=(BinStlSource&& other) noexcept = default;

std::uint64_t BinStlSource::recordCount() const
{
    return triangleCount() + 1; // the prefix, then one record per triangle
}

std::size_t BinStlSource::maxRecordSize() const
{
    return kPrefixSize;
}

std::size_t BinStlSource::renderRecord(std::uint64_t index, std::span<char> out)
{
    if (index == 0) {
        static constexpr char kHeader[] = "binary STL written by vine::meshio::BinStlSource";
        static_assert(sizeof(kHeader) - 1 <= kHeaderSize, "the header has to fit the 80 bytes binary STL reserves");

        std::memset(out.data(), 0, kPrefixSize);
        std::memcpy(out.data(), kHeader, sizeof(kHeader) - 1);

        const auto count = static_cast<std::uint32_t>(triangleCount());
        std::memcpy(out.data() + kHeaderSize, &count, sizeof(count));
        return kPrefixSize;
    }

    float a[3]{};
    float b[3]{};
    float c[3]{};
    if (!triangleAt(index - 1, a, b, c)) {
        return 0;
    }

    float normal[3]{};
    facetNormal(a, b, c, normal);

    putVec3(out.data(), 0, normal);
    putVec3(out.data(), 12, a);
    putVec3(out.data(), 24, b);
    putVec3(out.data(), 36, c);

    const std::uint16_t attribute = 0; // "the attribute byte count is zero" is what the format expects
    std::memcpy(out.data() + 48, &attribute, sizeof(attribute));
    return kTriangleSize;
}

VN_MESHIO_NS_END
