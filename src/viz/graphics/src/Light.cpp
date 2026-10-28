#include <vine/graphics/Light.hpp>

#include <algorithm>

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
    auto light        = make_intrusive<Light>();
    light->type_      = LightType::Directional;
    light->direction_ = direction;
    return light;
}

LightPtr Light::createPoint(const vn::math::Vec3d& position, float range)
{
    auto light       = make_intrusive<Light>();
    light->type_     = LightType::Point;
    light->position_ = position;
    light->range_    = range;
    return light;
}

LightPtr Light::createSpot(const vn::math::Vec3d& position, const vn::math::Vec3d& direction, double inner_angle, double outer_angle, float range)
{
    auto light        = make_intrusive<Light>();
    light->type_      = LightType::Spot;
    light->position_  = position;
    light->direction_ = direction;
    light->range_     = range;
    // OUTER first: the inner setter clamps against the outer one, so this order keeps the caller's intent
    // when the two arrive the other way round (see setInnerConeAngle).
    light->setOuterConeAngle(outer_angle);
    light->setInnerConeAngle(inner_angle);
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
    // A spot light is a directional light with a cone: both state a direction, and the packer writes it for
    // both (a point light's direction slot stays zero, which the shading reads as "no cone").
    return type_ == LightType::Directional || type_ == LightType::Spot;
}

vn::math::Vec3d Light::direction() const
{
    return direction_;
}

void Light::setDirection(const vn::math::Vec3d& direction)
{
    direction_ = direction;
}

bool Light::hasPosition() const
{
    return type_ == LightType::Point || type_ == LightType::Spot;
}

vn::math::Vec3d Light::position() const
{
    return position_;
}

void Light::setPosition(const vn::math::Vec3d& position)
{
    position_ = position;
}

float Light::range() const
{
    return range_;
}

void Light::setRange(float range)
{
    range_ = range;
}

double Light::innerConeAngle() const
{
    return inner_cone_degrees_;
}

void Light::setInnerConeAngle(double degrees)
{
    // Clamped against the OUTER angle, so the pair always describes a cone with a non-negative soft edge: a
    // caller that states them in the other order still gets a usable light (see setOuterConeAngle).
    inner_cone_degrees_ = std::clamp(degrees, 0.0, 90.0);
    if (inner_cone_degrees_ > outer_cone_degrees_) {
        inner_cone_degrees_ = outer_cone_degrees_;
    }
}

double Light::outerConeAngle() const
{
    return outer_cone_degrees_;
}

void Light::setOuterConeAngle(double degrees)
{
    outer_cone_degrees_ = std::clamp(degrees, 0.0, 90.0);
    if (outer_cone_degrees_ < inner_cone_degrees_) {
        inner_cone_degrees_ = outer_cone_degrees_;
    }
}

bool Light::isCastingShadow() const
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

VN_GRAPHICS_NS_END
