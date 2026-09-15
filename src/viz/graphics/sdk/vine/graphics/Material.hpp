#pragma once
#include "graphics_global.hpp"

#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/RefCounted.hpp>
#include <vine/Colorf.hpp>
#include <vine/String.hpp>

#include "Texture.hpp"

V_GRAPHICS_NS_BEGIN

/**
 * @brief Material class defining surface appearance.
 *
 * Describes the visual appearance of a geometry: diffuse/specular/ambient
 * colors, shininess and an optional texture. A material is always opaque:
 * transparency is a property of the leaf (Geometry::setOpacity) or of a
 * subtree (Node::setOpacity), never of the material itself. The diffuse color's
 * alpha is carried to the shader unchanged (VineMaterialBlock::diffuse.w) for a
 * host program that wants a transparency model of its own, but no engine program
 * reads it: the forward stage's fragment alpha is the drawable's opacity alone,
 * because the engine sorts by that value and a material is shared by every
 * drawable that uses it.
 */
class V_GRAPHICS_API Material : public Object, public RefCounted<Material> {
    V_OBJECT_META_DECL;

  public:
    Material();

  public:
    /** @brief Gets the material name. */
    String name() const;

    /** @brief Sets the material name. */
    void setName(const String& name);

    /** @brief Gets the diffuse color (RGBA). */
    Colorf diffuse() const;

    /** @brief Sets the diffuse color.
     *
     * The alpha is stored and passed to a host shader program through the material block; the
     * engine's own programs do not read it (see the class note), so setting it does not make the
     * object translucent - use Geometry::setOpacity() / Node::setOpacity() for that.
     *
     * @param color RGBA color in [0, 1].
     */
    void setDiffuse(const Colorf& color);

    /** @brief Gets the specular color (RGB, A is intensity). */
    Colorf specular() const;

    /** @brief Sets the specular color.
     *
     * @param color RGBA color in [0, 1].
     */
    void setSpecular(const Colorf& color);

    /** @brief Gets the ambient color (RGB). */
    Colorf ambient() const;

    /** @brief Sets the ambient color.
     *
     * @param color RGBA color in [0, 1].
     */
    void setAmbient(const Colorf& color);

    /** @brief Gets the shininess (Phong exponent). */
    float shininess() const;

    /** @brief Sets the shininess (Phong exponent).
     *
     * @param shine Phong exponent, typically [1, 128].
     */
    void setShininess(float shine);

    /** @brief Gets the texture this material samples.
     *
     * @return The texture, or null when the material has none.
     */
    raw_ptr<Texture> texture() const;

    /** @brief Sets the texture this material samples.
     *
     * @param texture The texture to sample, or null to clear it.
     */
    void setTexture(intrusive_ptr<Texture> texture);

  private:
    String              name_;
    Colorf              diffuse_{ 0.8f, 0.8f, 0.8f, 1.0f };
    Colorf              specular_{ 1.0f, 1.0f, 1.0f, 0.5f };
    Colorf              ambient_{ 0.2f, 0.2f, 0.2f, 1.0f };
    float               shininess_ = 32.0f;
    intrusive_ptr<Texture> texture_;
};

using MaterialPtr = intrusive_ptr<Material>;

V_GRAPHICS_NS_END
