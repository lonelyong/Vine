#pragma once

#include <span>
#include <vector>

#include <vine/graphics/Light.hpp>

/**
 * @brief The value half of @p lights: what the recording side reads (`LightRef`, see Light::value).
 *
 * @details
 * - THE NAMED CONVERSION, ON THE CALLER'S THREAD: a caller that holds `Light` objects converts them where it holds
 *   them - the recorder takes values only, so recording never touches a `Light` (see FrameRecorder::setLights).
 * - It lives in the test support on purpose: production callers are `RenderPass::describe` / `ScreenPass::describe`,
 *   which snapshot a scene's lights while they still hold it.
 *
 * @param lights The lights to convert; null entries keep their slot as the default disabled light.
 * @return One value per light, in order.
 */
inline std::vector<vn::graphics::LightValue> lightValues(std::span<const vn::graphics::Light* const> lights)
{
    std::vector<vn::graphics::LightValue> values;
    values.reserve(lights.size());
    for (const vn::graphics::Light* light : lights) {
        values.push_back(light != nullptr ? light->value() : vn::graphics::LightValue{});
    }
    return values;
}
