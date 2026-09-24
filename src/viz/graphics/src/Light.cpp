#include <vine/graphics/Light.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(Light, vn::Object);

Light::Light() = default;

String Light::name() const
{
    return name_;
}

void Light::setName(const String& name)
{
    name_ = name;
}

LightPtr Light::createAmbient()
{
    return make_intrusive<Light>();
}

LightPtr Light::createDirectional(const vn::math::Vec3d& direction)
{
    auto light = make_intrusive<Light>();
    light->type_ = LightType::Directional;
    light->direction_ = direction;
    return light;
}

LightType Light::type() const
{
    return type_;
}

bool Light::isEnabled() const
{
    return enabled_;
}

void Light::setEnabled(bool enabled)
{
    enabled_ = enabled;
}

Colorf Light::color() const
{
    return color_;
}

void Light::setColor(const Colorf& color)
{
    color_ = color;
}

float Light::intensity() const
{
    return intensity_;
}

void Light::setIntensity(float intensity)
{
    intensity_ = intensity;
}

bool Light::hasDirection() const
{
    return type_ == LightType::Directional;
}

vn::math::Vec3d Light::direction() const
{
    return direction_;
}

void Light::setDirection(const vn::math::Vec3d& direction)
{
    direction_ = direction;
}

bool Light::castShadow() const
{
    return cast_shadow_;
}

void Light::setCastShadow(bool cast)
{
    cast_shadow_ = cast;
}

const ShadowSettings& Light::shadowSettings() const
{
    return shadow_settings_;
}

void Light::setShadowSettings(const ShadowSettings& settings)
{
    shadow_settings_ = settings;
}

void Light::setShadowResolution(uint32_t resolution)
{
    shadow_settings_.resolution = resolution;
}

void Light::setShadowBias(float bias)
{
    shadow_settings_.bias = bias;
}

void Light::setShadowFilter(ShadowFilter filter)
{
    shadow_settings_.filter = filter;
}

VN_GRAPHICS_NS_END
