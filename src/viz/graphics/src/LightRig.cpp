#include <vine/graphics/LightRig.hpp>

#include <vine/graphics/Light.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/intrusive_ptr.hpp>

VN_GRAPHICS_NS_BEGIN

namespace
{

/// One normalized direction for the rigs below, written out so no vector math has to happen at load time.
constexpr vn::math::Vec3d kKeyDirection{ 0.42, -0.68, -0.60 };   // from up-front-left, travelling down-back-right
constexpr vn::math::Vec3d kFillDirection{ -0.55, -0.30, 0.78 };  // from the opposite side, dimmer and flatter

} // namespace

LightRig LightRig::studio()
{
    LightRig rig;
    // The ambient term is what keeps the shadow side readable; the two directions give the silhouette its shape.
    rig.lights_.push_back(Entry{ Colorf{ 0.25f, 0.25f, 0.28f }, 1.0f, false, vn::math::Vec3d{} });
    rig.lights_.push_back(Entry{ Colorf{ 1.0f, 0.98f, 0.95f }, 1.0f, true, kKeyDirection });
    rig.lights_.push_back(Entry{ Colorf{ 0.85f, 0.90f, 1.0f }, 0.35f, true, kFillDirection });
    return rig;
}

LightRig LightRig::headlight(const vn::math::Vec3d& direction)
{
    LightRig rig;
    // A zero direction would be a light that lights nothing: fall back to the default view direction instead of
    // handing the backend something it has to defend against.
    const vn::math::Vec3d travel = direction.length() > 0.0 ? direction : vn::math::Vec3d(0.0, 0.0, -1.0);
    rig.lights_.push_back(Entry{ Colorf{ 1.0f, 1.0f, 1.0f }, 1.0f, true, travel });
    return rig;
}

void LightRig::addTo(Scene& scene) const
{
    for (const Entry& entry : lights_) {
        intrusive_ptr<Light> light = entry.directional ? Light::createDirectional(entry.direction) : Light::createAmbient();
        if (light == nullptr) {
            continue;
        }
        light->setColor(entry.color);
        light->setIntensity(entry.intensity);
        scene.addLight(std::move(light));
    }
}

const std::vector<LightRig::Entry>& LightRig::lights() const noexcept
{
    return lights_;
}

VN_GRAPHICS_NS_END
