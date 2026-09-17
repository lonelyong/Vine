#include <vine/vsg/VsgLights.hpp>

#include <cmath>

#include <vine/vsg/VsgPipelineFactory.hpp>

V_VSG_NS_BEGIN

namespace detail
{

void viewRotation(const vine::graphics::Camera* camera, double r[3], double u[3], double f[3])
{
    const auto eye    = camera->eye();
    const auto center = camera->target();
    const auto up_vec = camera->up();
    double fx = center.x - eye.x;
    double fy = center.y - eye.y;
    double fz = center.z - eye.z;
    const double fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fl > 1e-12) {
        fx /= fl;
        fy /= fl;
        fz /= fl;
    }
    else {
        fx = 0.0;
        fy = 0.0;
        fz = -1.0;
    }
    // r = normalize(f x up), u = r x f.
    double rx = fy * up_vec.z - fz * up_vec.y;
    double ry = fz * up_vec.x - fx * up_vec.z;
    double rz = fx * up_vec.y - fy * up_vec.x;
    const double rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rl > 1e-12) {
        rx /= rl;
        ry /= rl;
        rz /= rl;
    }
    else {
        rx = 1.0;
        ry = 0.0;
        rz = 0.0;
    }
    const double ux = ry * fz - rz * fy;
    const double uy = rz * fx - rx * fz;
    const double uz = rx * fy - ry * fx;
    r[0] = rx;
    r[1] = ry;
    r[2] = rz;
    u[0] = ux;
    u[1] = uy;
    u[2] = uz;
    f[0] = fx;
    f[1] = fy;
    f[2] = fz;
}

namespace
{

/**
 * @brief Packs a light list into view-space ambient + up to three directional slots.
 *
 * One implementation for both consumers: the full-screen deferred path encodes
 * this into its push block (LightPushBlock) and the forward path into its
 * per-view uniform block (VineLightsBlock). Directions arrive in world space and
 * leave in view space (rotation only: rows r, u, -f), which is what lets both
 * shaders light in view space and skip the world matrix entirely.
 *
 * An empty (or entirely unusable) light list keeps a small default ambient so a
 * scene is still visible instead of being multiplied by zero.
 *
 * @param camera  Face rotation source (null leaves the block empty).
 * @param lights  Lights to pack (borrowed; null entries and disabled lights are skipped).
 * @param ambient Receives rgb + intensity.
 * @param dirs    Receives up to three view-space directions (xyz, w = 0).
 * @param cols    Receives rgb + intensity per direction.
 * @return How many of @p lights the packing represents (the caller reports the difference).
 */
std::size_t collectViewSpaceLights(const vine::graphics::Camera*                    camera,
                                   const std::vector<const vine::graphics::Light*>& lights,
                                   std::array<float, 4>&                            ambient,
                                   std::array<std::array<float, 4>, 3>&             dirs,
                                   std::array<std::array<float, 4>, 3>&             cols)
{
    if (camera == nullptr) {
        return 0u;
    }
    double r[3] = {}, u[3] = {}, f[3] = {};
    viewRotation(camera, r, u, f);
    int  dirlight    = 0;
    bool has_ambient = false;
    for (const auto* light : lights) {
        if (light == nullptr || !light->isEnabled()) {
            continue;
        }
        const auto c = light->color();
        switch (light->type()) {
        case vine::graphics::LightType::Ambient:
            ambient[0]  = c.r;
            ambient[1]  = c.g;
            ambient[2]  = c.b;
            ambient[3]  = light->intensity();
            has_ambient = true;
            break;
        case vine::graphics::LightType::Directional:
            if (dirlight >= 3) {
                break; // the block holds up to three directional lights
            }
            {
                const auto d = light->direction();
                // world -> view direction (rotation only): rows r, u, -f.
                double vx = r[0] * d.x + r[1] * d.y + r[2] * d.z;
                double vy = u[0] * d.x + u[1] * d.y + u[2] * d.z;
                double vz = -f[0] * d.x - f[1] * d.y - f[2] * d.z;
                const double vl = std::sqrt(vx * vx + vy * vy + vz * vz);
                if (vl > 1e-9) {
                    vx /= vl;
                    vy /= vl;
                    vz /= vl;
                }
                float* dd = dirs[dirlight].data();
                float* cc = cols[dirlight].data();
                dd[0] = static_cast<float>(vx);
                dd[1] = static_cast<float>(vy);
                dd[2] = static_cast<float>(vz);
                dd[3] = 0.0f;
                cc[0] = c.r;
                cc[1] = c.g;
                cc[2] = c.b;
                cc[3] = light->intensity();
                ++dirlight;
            }
            break;
        default:
            break;
        }
    }
    if (!has_ambient) {
        // Keep an unlit pass visible: without any ambient the fragment shader
        // would multiply the albedo by zero (see the header).
        ambient[0] = 0.15f;
        ambient[1] = 0.15f;
        ambient[2] = 0.15f;
        ambient[3] = 1.0f;
    }
    // The ambient FILL above is not an announced light, so it is not counted: the number has to be
    // about what the host asked for, or the caller reports a drop that did not happen.
    return static_cast<std::size_t>(dirlight) + (has_ambient ? 1u : 0u);
}

} // namespace

void fillLightPushBlock(const vine::graphics::Camera*                    camera,
                        const std::vector<const vine::graphics::Light*>& lights, LightPushBlock& block)
{
    block = LightPushBlock{};
    if (camera == nullptr) {
        return;
    }
    // Perspective projection parameters for view-position reconstruction from
    // the G-buffer depth (near / far / proj00 / proj11).
    if (camera->projectionType() == vine::graphics::Camera::ProjectionType::Perspective) {
        const double fov    = camera->fieldOfView() * 0.5; // degrees
        const double cot    = 1.0 / std::tan(fov * 3.14159265358979323846 / 180.0);
        const double aspect = camera->aspectRatio();
        block.projparms[0]  = static_cast<float>(camera->nearPlane());
        block.projparms[1]  = static_cast<float>(camera->farPlane());
        block.projparms[2]  = static_cast<float>(cot / aspect); // proj[0][0]
        block.projparms[3]  = static_cast<float>(cot);          // proj[1][1]
    }
    collectViewSpaceLights(camera, lights, block.ambient, block.dirs, block.cols);
}

std::size_t fillVineLightsBlock(const vine::graphics::Camera*                    camera,
                                const std::vector<const vine::graphics::Light*>& lights, VineLightsBlock& block)
{
    block = VineLightsBlock{};
    return collectViewSpaceLights(camera, lights, block.ambient, block.dirs, block.cols);
}

ShadowLightSlot shadowLightSlot(const std::vector<const vine::graphics::Light*>& lights)
{
    // The same walk collectViewSpaceLights makes: enabled directionals take slots 0..2 in announcement
    // order, everything else is skipped, and a fourth directional is not lit at all. The shadow block's
    // index has to mean the same slot the light block put the light in, or the term would scale another
    // light (see the declaration).
    ShadowLightSlot found;
    std::size_t     slot = 0u;
    for (const auto* light : lights) {
        if (light == nullptr || !light->isEnabled() || light->type() != vine::graphics::LightType::Directional) {
            continue;
        }
        if (light->castShadow()) {
            found.light = light;
            found.slot  = slot;
            return found;
        }
        if (++slot >= 3u) {
            break; // the block is full: a later caster has no slot a shader could name it by
        }
    }
    return found;
}

} // namespace detail

V_VSG_NS_END
