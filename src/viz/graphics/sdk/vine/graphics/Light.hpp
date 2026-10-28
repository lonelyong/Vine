#pragma once
#include "graphics_global.hpp"

#include <vine/Colorf.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Vector3.hpp>

VN_GRAPHICS_NS_BEGIN

class Light;
using LightPtr = intrusive_ptr<Light>;

/**
 * @brief Identifies the kind of a light source.
 *
 * @details
 * - The VALUES are the shading ABI's light tags (see `VineLightsBlock` in `shaders/vine_params.glsl`): the
 *   packed block stores one as a float per slot, so renumbering the enumerators would silently turn every
 *   point light into a directional one. They are written out for that reason, not for style.
 */
enum class LightType
{
    Ambient     = 0, // Constant, directionless fill light (not packed into the lights array).
    Directional = 1, // Parallel light from an infinite distance (e.g. the sun).
    Point       = 2, // Omni-directional light from a position.
    Spot        = 3, // Cone light from a position, along a direction.
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
    uint32_t resolution = 1024;   // Shadow-map side length in texels.
    float    bias       = 0.002f; // Depth bias used to avoid self-shadowing.
};

/**
 * @brief A light source attached to a Scene.
 *
 * @details
 * - Lights live at SCENE level (not on a pass or a geometry node): a pass lights whatever scene it renders,
 *   so the same content is lit identically in the main, off-screen and shadow passes.
 * - All four kinds are shaded: directional (infinite, direction only), point (position + range), spot
 *   (position + direction + cone) and the ambient fill. A positional light's contribution falls to zero at
 *   its range, so the range is how a host bounds the cost of one light.
 * - SHADOWS REMAIN DIRECTIONAL: only a directional caster gets a depth pass (its light camera is
 *   orthographic), so `setCastShadow` on a point or spot light has no effect today and the host's diagnostic
 *   channel says so.
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
     * @return The new light (white, full intensity, enabled).
     */
    static LightPtr createDirectional(const vn::math::Vec3d& direction = vn::math::Vec3d(0.0, 0.0, -1.0));

    /** @brief Creates a point light at @p position.
     *
     * @param position World-space position.
     * @param range    Distance at which the light's contribution reaches zero (see setRange).
     * @return The new light (white, full intensity, enabled).
     */
    static LightPtr createPoint(const vn::math::Vec3d& position = vn::math::Vec3d(0.0, 0.0, 0.0), float range = 10.0F);

    /** @brief Creates a spot light at @p position, shining along @p direction.
     *
     * @details
     * - The cone is stated the way a UI states it: degrees from the axis, with a soft edge between the two.
     *
     * @param position    World-space position.
     * @param direction   World-space propagation direction.
     * @param inner_angle Angle from the axis where the cone is still at full strength (degrees).
     * @param outer_angle Angle from the axis where it has fallen to zero (degrees, > inner_angle).
     * @param range       Distance at which the contribution reaches zero (see setRange).
     * @return The new light (white, full intensity, enabled).
     */
    static LightPtr
    createSpot(const vn::math::Vec3d& position, const vn::math::Vec3d& direction, double inner_angle = 15.0, double outer_angle = 30.0, float range = 10.0F);

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
     * @return true for directional and spot lights.
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

    /** @brief Returns whether the light has a position (point and spot lights).
     *
     * @return true for point and spot lights.
     */
    bool hasPosition() const;

    /** @brief Gets the light position (world space).
     *
     * @return The position; only meaningful for point and spot lights.
     */
    vn::math::Vec3d position() const;

    /** @brief Sets the light position (world space).
     *
     * @param position World-space position.
     */
    void setPosition(const vn::math::Vec3d& position);

    /** @brief Gets the range at which a positional light's contribution reaches zero.
     *
     * @return The range in world units, or 0 when the light has no cutoff.
     */
    float range() const;

    /** @brief Sets the range at which a positional light's contribution reaches zero.
     *
     * @details
     * - The shading falls off with distance and is exactly zero at the range, which is what lets a host bound
     *   the cost of a light instead of lighting the whole scene with it.
     * - 0 (or negative) means NO cutoff: the light follows an inverse-square falloff everywhere. A finite range
     *   is what a host normally wants, and the default a factory states.
     *
     * @param range Range in world units (<= 0 for no cutoff).
     */
    void setRange(float range);

    /** @brief Gets the spot cone's inner angle (degrees from the axis).
     *
     * @return The angle at which the cone is still at full strength.
     */
    double innerConeAngle() const;

    /** @brief Sets the spot cone's inner angle (degrees from the axis).
     *
     * @param degrees Angle at which the cone is still at full strength (0..outerConeAngle).
     */
    void setInnerConeAngle(double degrees);

    /** @brief Gets the spot cone's outer angle (degrees from the axis).
     *
     * @return The angle at which the cone has fallen to zero.
     */
    double outerConeAngle() const;

    /** @brief Sets the spot cone's outer angle (degrees from the axis).
     *
     * @details
     * - Clamped to (innerConeAngle, 90]: a cone wider than a hemisphere is a point light with a direction, and a
     *   soft edge needs a positive width.
     *
     * @param degrees Angle at which the cone has fallen to zero.
     */
    void setOuterConeAngle(double degrees);

    /** @brief Returns whether the light casts a shadow (directional, v4b).
     *
     * @return true when shadow casting is requested.
     */
    bool isCastingShadow() const;

    /** @brief Sets whether the light casts a shadow.
     *
     * @details
     * - Read by PassChain::build, which builds ONE depth-only pass for the FIRST enabled, shadow-casting
     *   DIRECTIONAL or SPOT light of the content it was given (a directional light gets an orthographic light
     *   camera, a spot a perspective one taken from its cone; further casters, and content rendered by passes it
     *   did not create, stay unshadowed and are reported on the host's diagnostic channel).
     * - A POINT light has no pass yet (it needs six faces of one shadow - see backlog S6), so setting this on one
     *   has no effect and is reported.
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
    String          name_;
    LightType       type_{ LightType::Ambient };
    bool            enabled_{ true };
    Colorf          color_{ 1.0f, 1.0f, 1.0f, 1.0f };
    float           intensity_{ 1.0f };
    vn::math::Vec3d direction_{ 0.0, 0.0, -1.0 };
    vn::math::Vec3d position_{ 0.0, 0.0, 0.0 };
    float           range_{ 0.0f };
    double          inner_cone_degrees_{ 15.0 };
    double          outer_cone_degrees_{ 30.0 };
    bool            cast_shadow_{ false };
    ShadowSettings  shadow_settings_;
};

VN_GRAPHICS_NS_END
