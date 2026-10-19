#pragma once
#include "graphics_global.hpp"

#include <cstdint>

#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/RefCounted.hpp>
#include <vine/Colorf.hpp>
#include <vine/String.hpp>

#include "Texture.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Material class defining surface appearance.
 *
 * @details
 * - Diffuse / specular / ambient colours, shininess and an optional texture.
 * - A material is always OPAQUE: transparency is the leaf's or the subtree's (Geometry::setOpacity /
 *   Node::setOpacity), never the material's - the diffuse alpha travels to the shader unchanged
 *   (VineMaterialBlock::diffuse.w) for a host program with a transparency model of its own, but no engine
 *   program reads it, because the engine sorts by the drawable's opacity and a material is shared by every
 *   drawable that uses it.
 */
class VN_GRAPHICS_API Material : public Object, public RefCounted<Material> {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs a material with its default colours and no texture. */
    Material();

  public:
    /** @brief Gets the material name.
     *
     * @return The name (empty when never set).
     */
    String name() const;

    /** @brief Sets the material name.
     *
     * @param name New material name.
     */
    void setName(const String& name);

    /** @brief Gets the diffuse color (RGBA).
     *
     * @return The diffuse colour.
     */
    Colorf diffuse() const;

    /** @brief Sets the diffuse color.
     *
     * @details
     * - The alpha is stored and passed to a host shader program through the material block; the engine's
     *   own programs do not read it (see the class note), so setting it does NOT make the object
     *   translucent - use Geometry::setOpacity() / Node::setOpacity() for that.
     *
     * @param color RGBA color in [0, 1].
     */
    void setDiffuse(const Colorf& color);

    /** @brief Gets the specular color (RGB, A is intensity).
     *
     * @details
     * - The intensity multiplies the highlight in every engine program that shades one - the forward lit
     *   program and the deferred chain alike (the G-buffer's specular attachment carries the colour already
     *   scaled by it) - so an alpha of 0.5 leaves the surface half as reflective as the colour alone.
     *   Wired 2026-09-25: the accessor documented this from the first release, and no shader read it until
     *   then.
     *
     * @return The specular colour (RGBA).
     */
    Colorf specular() const;

    /** @brief Sets the specular color.
     *
     * @param color RGB in [0, 1] is the highlight's colour; A in [0, 1] scales its strength.
     */
    void setSpecular(const Colorf& color);

    /** @brief Gets the ambient color (RGB).
     *
     * @return The ambient colour.
     */
    Colorf ambient() const;

    /** @brief Sets the ambient color.
     *
     * @param color RGBA color in [0, 1].
     */
    void setAmbient(const Colorf& color);

    /** @brief Gets the shininess (Phong exponent).
     *
     * @return The Phong exponent.
     */
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

    /** @brief Gets the revision, which moves whenever a shading value of this material changes.
     *
     * @details
     * - THE MATERIAL MOVES ITS OWN REVISION. The other content types are announced by their WRITER instead
     *   (`Geometry::setRevision()`), because their bytes sit in buffers a caller can refill behind the SDK's
     *   back - a material has no such hole: every one of its shading values travels through a setter, so the
     *   setter can report the change itself and nobody has to remember to.
     * - WHAT MOVES IT: the diffuse / specular / ambient colours, the shininess and the texture - each only when
     *   the value actually DIFFERS (assigning what a material already holds is not a change, so a host that
     *   re-applies its settings every frame costs nothing). The NAME does not move it: a label is not shading.
     * - WHAT READS IT: a consumer that answers for this material caches on the number instead of reading the
     *   values back every frame (see the backend's ContentStore::updateMaterial).
     *
     * @return Monotonic revision, 0 until the first change.
     */
    std::uint64_t revision() const;

  private:
    String              name_;
    Colorf              diffuse_{ 0.8f, 0.8f, 0.8f, 1.0f };
    Colorf              specular_{ 1.0f, 1.0f, 1.0f, 0.5f };
    Colorf              ambient_{ 0.2f, 0.2f, 0.2f, 1.0f };
    float               shininess_ = 32.0f;
    intrusive_ptr<Texture> texture_;
    /// Moves when a shading value changes (see revision()); the name is deliberately not part of it.
    std::uint64_t revision_ = 0;
};

using MaterialPtr = intrusive_ptr<Material>;

VN_GRAPHICS_NS_END
