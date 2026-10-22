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
 * @brief Per-light shadow-map settings (value type).
 *
 * @details
 * - `resolution` and `bias` are consumed by the renderers that build and sample a shadow map.
 *
 * @note
 * - There is deliberately NO sampling-filter knob: the shadow term does one depth compare (see
 *   ShaderAbi.hpp's VineShadowBlock), so a filter setting would change nothing while reading as if it
 *   did. A filter arrives together with the implementation that honours it, not before.
 */
struct VN_GRAPHICS_API ShadowSettings {
    uint32_t resolution = 1024;   ///< Shadow-map side length in texels.
    float    bias       = 0.002f; ///< Depth bias used to avoid self-shadowing.
};

/**
 * @brief A light source attached to a Scene.
 *
 * @details
 * - Lights live at SCENE level (not on a pass or a geometry node): a pass lights whatever scene it renders,
 *   so the same content is lit identically in the main, off-screen and shadow passes.
 * - Ambient and Directional are shipped; Point / Spot and attachable (node-level) lights are planned later.
 */
class VN_GRAPHICS_API Light : public Object, public RefCounted<Light> {
    VN_OBJECT_META_DECL;
    VN_DISABLE_COPY_MOVE(Light);

  public:
    /** @brief Constructs an ambient light (white, full intensity). */
    Light();

    /** @brief Creates an ambient light.
     *
     * @return The new light (white, full intensity, enabled).
     */
    static LightPtr createAmbient();

    /** @brief Creates a directional light.
     *
     * @param direction Light propagation direction (world space).
     */
    static LightPtr createDirectional(const vn::math::Vec3d& direction = vn::math::Vec3d(0.0, 0.0, -1.0));

  public:
    /** @brief Gets the light name.
     *
     * @return The name (empty when never set).
     */
    String name() const;

    /** @brief Sets the light name.
     *
     * @param name Light name.
     */
    void setName(const String& name);

    /** @brief Gets the light's type.
     *
     * @return The light type (see LightType).
     */
    LightType type() const;

    /** @brief Returns whether the light contributes to shading.
     *
     * @return true when the light is enabled.
     */
    bool isEnabled() const;

    /** @brief Sets whether the light contributes to shading.
     *
     * @param enabled True to enable the light (the default).
     */
    void setEnabled(bool enabled);

    /** @brief Gets the light colour (RGB, A ignored).
     *
     * @return The colour.
     */
    Colorf color() const;

    /** @brief Sets the light colour.
     *
     * @param color RGB colour with components in [0, 1].
     */
    void setColor(const Colorf& color);

    /** @brief Gets the light intensity multiplier.
     *
     * @return The multiplier (1 is neutral).
     */
    float intensity() const;

    /** @brief Sets the light intensity multiplier.
     *
     * @param intensity Multiplier; 1 is the neutral value.
     */
    void setIntensity(float intensity);

    /** @brief Returns whether the light is directional (has a direction).
     *
     * @return true for directional lights.
     */
    bool hasDirection() const;

    /** @brief Gets the light propagation direction (world space).
     *
     * @details
     * - Only meaningful for directional lights.
     *
     * @return The direction.
     */
    vn::math::Vec3d direction() const;

    /** @brief Sets the light propagation direction (world space).
     *
     * @param direction Direction the light travels towards.
     */
    void setDirection(const vn::math::Vec3d& direction);

    /** @brief Returns whether the light casts a shadow (directional, v4b).
     *
     * @return true when shadow casting is requested.
     */
    bool isCastingShadow() const;

    /** @brief Sets whether the light casts a shadow.
     *
     * @details
     * - Read by PassChain::build, which builds ONE depth-only pass for the FIRST enabled, shadow-casting
     *   directional light of the content it was given (further ones, and content rendered by passes it did not
     *   create, stay unshadowed and are reported on the host's diagnostic channel).
     * - The engine itself schedules nothing: a shadow pass is an ordinary registered pass at
     *   PassStage::Depth.
     *
     * @param cast True to request shadow casting.
     */
    void setCastShadow(bool cast);

    /** @brief Gets the shadow-map settings (resolution / bias / filter).
     *
     * @return The current settings.
     */
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
