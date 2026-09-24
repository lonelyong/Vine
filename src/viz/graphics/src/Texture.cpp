#include <vine/graphics/Texture.hpp>

#include <stdexcept>

VN_GRAPHICS_NS_BEGIN

namespace
{

/**
 * @brief Gets how many images a kind is made of.
 *
 * @param kind Kind to count.
 * @return The face count of the kind.
 */
std::size_t faceCountOf(Texture::Kind kind) noexcept
{
    return (kind == Texture::Kind::Cube) ? 6u : 1u;
}

} // namespace

VN_OBJECT_META_IMPL(Texture, vn::Object);

const char* Texture::kindName(Kind kind) noexcept
{
    switch (kind) {
        case Kind::D2:
            return "D2";
        case Kind::Cube:
            return "Cube";
    }

    return "Unknown"; // Also the answer for an out-of-range cast.
}

Texture::Texture(Kind kind, int width, int height, imaging::PixelFormat format, int mip_count)
  : width_(width)
  , height_(height)
  , format_(format)
  , mip_count_(mip_count)
  , kind_(kind)
{
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("Texture: width and height must be positive");
    }

    if (imaging::bytesPerPixel(format) == 0) {
        throw std::invalid_argument("Texture: not a pixel layout (PixelFormat::Unknown or out of range)");
    }

    // The mip limit is the image's own rule, reused rather than restated: a texture that could hold more
    // levels than an image of the same size would be a description no source image could ever fill.
    if (mip_count < 1 || mip_count > imaging::Image::mipCapacity(width, height)) {
        throw std::invalid_argument("Texture: mip count must lie in [1, Image::mipCapacity(width, height)]");
    }

    sources_.resize(faceCountOf(kind));
}

int Texture::faceCount() const noexcept
{
    return static_cast<int>(sources_.size());
}

Texture::Kind Texture::kind() const noexcept
{
    return kind_;
}

int Texture::width() const noexcept
{
    return width_;
}

int Texture::height() const noexcept
{
    return height_;
}

imaging::PixelFormat Texture::format() const noexcept
{
    return format_;
}

int Texture::mipCount() const noexcept
{
    return mip_count_;
}

void Texture::setSource(int face, intrusive_ptr<const imaging::Image> image)
{
    if (face < 0 || face >= faceCount()) {
        throw std::out_of_range("Texture: face index is outside this kind");
    }

    if (image != nullptr) {
        if (image->format() != format_) {
            throw std::invalid_argument("Texture: the source image's format does not match the texture");
        }
        if (image->width() != width_ || image->height() != height_) {
            throw std::invalid_argument("Texture: the source image's size does not match the texture");
        }
        if (image->mipCount() != mip_count_) {
            throw std::invalid_argument("Texture: the source image's mip count does not match the texture");
        }
    }

    sources_[static_cast<std::size_t>(face)] = image;
    ++revision_;
}

raw_ptr<const imaging::Image> Texture::source(int face) const noexcept
{
    if (face < 0 || face >= faceCount()) {
        return nullptr;
    }

    return sources_[static_cast<std::size_t>(face)].get();
}

bool Texture::hasSource(int face) const noexcept
{
    return source(face) != nullptr;
}

bool Texture::isComplete() const noexcept
{
    for (const intrusive_ptr<const imaging::Image>& source : sources_) {
        if (source == nullptr) {
            return false;
        }
    }

    return true;
}

std::uint64_t Texture::revision() const noexcept
{
    return revision_;
}

int Texture::layerCount() const noexcept
{
    return static_cast<int>(sources_.size());
}

raw_ptr<const imaging::Image> Texture::layer(int index) const noexcept
{
    return source(index);
}

VN_OBJECT_META_IMPL(Texture2D, Texture);

Texture2D::Texture2D(int width, int height, imaging::PixelFormat format, int mip_count)
  : Texture(Kind::D2, width, height, format, mip_count)
{
}

Texture::Kind Texture2D::kind() const noexcept
{
    return Kind::D2;
}

void Texture2D::setImage(intrusive_ptr<const imaging::Image> image)
{
    setSource(0, image);
}

raw_ptr<const imaging::Image> Texture2D::image() const noexcept
{
    return source(0);
}

VN_OBJECT_META_IMPL(CubeMap, Texture);

const char* CubeMap::faceName(Face face) noexcept
{
    switch (face) {
        case Face::PosX:
            return "+X";
        case Face::NegX:
            return "-X";
        case Face::PosY:
            return "+Y";
        case Face::NegY:
            return "-Y";
        case Face::PosZ:
            return "+Z";
        case Face::NegZ:
            return "-Z";
    }

    return "Unknown"; // Also the answer for an out-of-range cast.
}

CubeMap::CubeMap(int size, imaging::PixelFormat format, int mip_count)
  : Texture(Kind::Cube, size, size, format, mip_count)
{
}

Texture::Kind CubeMap::kind() const noexcept
{
    return Kind::Cube;
}

void CubeMap::setFaceImage(Face face, intrusive_ptr<const imaging::Image> image)
{
    setSource(static_cast<int>(face), image);
}

raw_ptr<const imaging::Image> CubeMap::faceImage(Face face) const noexcept
{
    return source(static_cast<int>(face));
}

VN_GRAPHICS_NS_END
