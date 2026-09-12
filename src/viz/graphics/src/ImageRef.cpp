#include <vine/graphics/ImageRef.hpp>

V_GRAPHICS_NS_BEGIN

V_OBJECT_META_IMPL(ImageRef, vine::Object);

ImageRef::ImageRef(const String& label, Kind kind) : label_(label), kind_(kind) {}

const String& ImageRef::label() const noexcept
{
    return label_;
}

ImageRef::Kind ImageRef::kind() const noexcept
{
    return kind_;
}

void ImageRef::bind(intrusive_ptr<RenderTarget> target, int attachment)
{
    target_     = target;
    attachment_ = (target_ != nullptr) ? attachment : 0;
}

void ImageRef::unbind()
{
    target_     = nullptr;
    attachment_ = 0;
}

raw_ptr<RenderTarget> ImageRef::target() const noexcept
{
    return target_.get();
}

int ImageRef::attachment() const noexcept
{
    return attachment_;
}

bool ImageRef::bound() const noexcept
{
    return target_ != nullptr;
}

V_GRAPHICS_NS_END
