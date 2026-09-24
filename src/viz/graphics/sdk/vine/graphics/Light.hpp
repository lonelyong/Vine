#pragma once
#include "graphics_global.hpp"

#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/Colorf.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/String.hpp>

VN_GRAPHICS_NS_BEGIN

class Light;
using LightPtr = intrusive_ptr<Light>;

/**
 * @brief Identifies the kind of a light source.
 */
enum class LightType {
    Ambient,     ///< Constant, directionless fill light.
    Directional, ///< Parallel light from an infinite distance (e.g. the sun).
    Point,       ///< Omni-directional light from a position (reserved).
    Spot,        ///< Cone light from a position (reserved).
};

/**
 * @brief Shadow sampling filter for a directional light.
 */
enum class ShadowFilter {
    None, ///< No shadow is sampled.
    Hard, ///< Single depth compare (hard-edged shadow).
    PCF,  ///< Percentage-closer filtering (soft-edged shadow).
};

/**
 * @brief Per-light shadow-map settings (value type).
 *
 * `resolution` and `bias` are consumed by the renderers that build and sample a shadow map;
 * `filter` is RESERVED — no renderer reads it yet (the shadow term does one depth compare, see
 * ShaderAbi.hpp's VineShadowBlock), so setting it changes nothing today. It is kept so a host can
 * state the intent a future PCF implementation will honour, and marked here so it cannot be
 * mistaken for a working quality knob.
 */
struct VN_GRAPHICS_API ShadowSettings {
    uint32_t     resolution = 1024;                    ///< Shadow-map side length in texels.
    float        bias       = 0.002f;                  ///< Depth bias used to avoid self-shadowing.
    ShadowFilter filter     = ShadowFilter::Hard;      ///< Sampling filter (RESERVED — not read yet).
};

/**
 * @brief A light source attached to a Scene.
 *
 * Lights live at scene level (not on a pass or on a geometry node): a pass
 * lights whatever scene it renders, so the same content lit identically in
 * the main, off-screen or shadow passes. v4 ships Ambient and Directional
 * lights; Point/Spot and attachable (node-level) lights are planned later.
 */
class VN_GRAPHICS_API Light : public Object, public RefCounted<Light> {
    VN_OBJECT_META_DECL;
    VN_DISABLE_COPY_MOVE(Light);

  public:
    /** @brief Constructs an ambient light (white, full intensity). */
    Light();

    /** @brief Creates an ambient light. */
    static LightPtr createAmbient();

    /** @brief Creates a directional light.
     *
     * @param direction Light propagation direction (world space).
     */
    static LightPtr createDirectional(const vn::math::Vec3d& direction = vn::math::Vec3d(0.0, 0.0, -1.0));

  public:
    /** @brief Gets the light name. */
    String name() const;

    /** @brief Sets the light name.
     *
     * @param name Light name.
     */
    void setName(const String& name);

    /** @brief Gets the light's type. */
    LightType type() const;

    /** @brief Returns whether the light contributes to shading. */
    bool isEnabled() const;

    /** @brief Sets whether the light contributes to shading.
     *
     * @param enabled True to enable the light (the default).
     */
    void setEnabled(bool enabled);

    /** @brief Gets the light colour (RGB, A ignored). */
    Colorf color() const;

    /** @brief Sets the light colour.
     *
     * @param color RGB colour with components in [0, 1].
     */
    void setColor(const Colorf& color);

    /** @brief Gets the light intensity multiplier. */
    float intensity() const;

    /** @brief Sets the light intensity multiplier.
     *
     * @param intensity Multiplier; 1 is the neutral value.
     */
    void setIntensity(float intensity);

    /** @brief Returns whether the light is directional (has a direction). */
    bool hasDirection() const;

    /** @brief Gets the light propagation direction (world space).
     *
     * Only meaningful for directional lights.
     */
    vn::math::Vec3d direction() const;

    /** @brief Sets the light propagation direction (world space).
     *
     * @param direction Direction the light travels towards.
     */
    void setDirection(const vn::math::Vec3d& direction);

    /** @brief Returns whether the light casts a shadow (directional, v4b). */
    bool castShadow() const;

    /** @brief Sets whether the light casts a shadow.
     *
     * Read by RenderPipelineBuilder, which builds ONE depth-only pass for the FIRST enabled,
     * shadow-casting directional light of the content it was given (the further ones, and any
     * content rendered by passes it did not create, stay unshadowed and are reported on the host's
     * diagnostic channel). The engine itself schedules nothing: a shadow pass is an ordinary
     * registered pass at PipelineStage::Depth.
     *
     * @param cast True to request shadow casting.
     */
    void setCastShadow(bool cast);

    /** @brief Gets the shadow-map settings (resolution / bias / filter). */
    const ShadowSettings& shadowSettings() const;

    /** @brief Sets the shadow-map settings.
     *
     * @param settings Shadow map configuration.
     */
    void setShadowSettings(const ShadowSettings& settings);

    /** @brief Sets the shadow-map resolution.
     *
     * @param resolution Shadow-map side length in texels.
     */
    void setShadowResolution(uint32_t resolution);

    /** @brief Sets the shadow depth bias.
     *
     * @param bias Depth bias (positive pushes the comparison away from self
     *             shadowing).
     */
    void setShadowBias(float bias);

    /** @brief Sets the shadow sampling filter.
     *
     * @param filter Shadow filter (None / Hard / PCF).
     */
    void setShadowFilter(ShadowFilter filter);

  private:
    String                              name_;
    LightType                           type_{ LightType::Ambient };
    bool                                enabled_{ true };
    Colorf                              color_{ 1.0f, 1.0f, 1.0f, 1.0f };
    float                               intensity_{ 1.0f };
    vn::math::Vec3d                   direction_{ 0.0, 0.0, -1.0 };
    bool                                cast_shadow_{ false };
    ShadowSettings                      shadow_settings_;
};

VN_GRAPHICS_NS_END
