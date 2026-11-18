#include <vine/graphics/MaterialValues.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(MaterialValues, vn::Object);

MaterialValues::MaterialValues()
  : id_(ContentRegistry::instance().allocate<MaterialTag>())
{}

MaterialValues::~MaterialValues()
{
    // The id leaves with the object: a plan or a facts row that still names it afterwards reads "gone" instead of
    // resolving to whatever the slot holds next (see ContentId.hpp).
    ContentRegistry::instance().release(id_.handle());
}

MaterialId MaterialValues::id() const noexcept
{
    return id_;
}

String MaterialValues::name() const
{
    return name_;
}

void MaterialValues::setName(const String& name)
{
    // A NAME IS NOT SHADING: it never moves the revision (see revision()), so renaming a material cannot make a
    // consumer rebuild what it already answered.
    name_ = name;
}

Colorf MaterialValues::baseColor() const noexcept
{
    return resolved().base_color;
}

Colorf MaterialValues::emissive() const noexcept
{
    return resolved().emissive;
}

float MaterialValues::metallic() const noexcept
{
    return resolved().metallic;
}

float MaterialValues::roughness() const noexcept
{
    return resolved().roughness;
}

float MaterialValues::occlusion() const noexcept
{
    return resolved().occlusion;
}

float MaterialValues::alphaCutoff() const noexcept
{
    return resolved().alpha_cutoff;
}

raw_ptr<Texture> MaterialValues::texture(TextureSlot slot) const noexcept
{
    return resolved().textures[static_cast<std::size_t>(slot)].texture.get();
}

const Sampler& MaterialValues::sampler(TextureSlot slot) const noexcept
{
    return resolved().textures[static_cast<std::size_t>(slot)].sampler;
}

bool MaterialValues::hasTexture(TextureSlot slot) const noexcept
{
    return resolved().textures[static_cast<std::size_t>(slot)].texture != nullptr;
}

VN_GRAPHICS_NS_END
