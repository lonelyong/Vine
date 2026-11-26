#pragma once

#include <optional>

#include <vine/models/models_global.hpp>

#include <vine/INameable.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>

VN_MODELS_NS_BEGIN

/**
 * @brief Concrete material category.
 */
enum class MaterialType
{
    /// Not a concrete material yet.
    Unknown = 0,
    /// Single base color (ColorMaterial).
    Color,
    /// Phong shading (PhongMaterial).
    Phong,
    /// Physically based rendering (PbrMaterial).
    Pbr,
};

/**
 * @brief Base class for surface materials applied to shapes.
 *
 * Every material carries a name: a loader fills it from the source model, so a
 * material that came out of a file keeps the identity it was declared with.
 * The name is free-form and may be empty.
 */
class VN_MODELS_API Material : public vn::Object, public vn::RefCounted<Material>, public vn::INameable {
    VN_OBJECT_META_DECL;

  public:
    Material();

  public:
    /**
     * @brief Returns the material name.
     *
     * @return The name; empty when the material was never named.
     */
    const String& name() const noexcept override
    {
        return name_;
    }

    /**
     * @brief Sets the material name.
     *
     * @param name The new name; may be empty.
     */
    void setName(const String& name) override
    {
        name_ = name;
    }

    /**
     * @brief Returns the concrete material category.
     *
     * @return The MaterialType of this material.
     */
    [[nodiscard]]
    MaterialType materialType() const
    {
        return material_type_;
    }

    /**
     * @brief Returns the concrete material type name.
     *
     * @return Type name, e.g. "ColorMaterial".
     */
    [[nodiscard]]
    virtual const char* typeName() const = 0;

    /**
     * @brief Returns whether the SOURCE stated whether both sides of the surface are visible.
     *
     * @details
     * - THREE ANSWERS, NOT TWO: a format that states the fact (glTF's `material.doubleSided`) says `true`, a format
     *   that states nothing (an OBJ/MTL, a plain colour) says nothing - and "nothing" is not "false", because a
     *   consumer that culls by default would then turn every unstated sheet single-sided.
     * - The fact belongs to the MODEL, not to a renderer: it is what the file said about its own surface, and the
     *   graphics conversion is the one place that turns it into a cull mode (see SceneBuild / graphics-scene.md).
     *
     * @return true when the source stated either value; false when it stated nothing.
     */
    [[nodiscard]] bool hasDoubleSided() const noexcept
    {
        return double_sided_.has_value();
    }

    /**
     * @brief Returns the source's two-sided statement; false when it stated nothing.
     *
     * @return Whether both sides are visible; false for an unstated fact (read hasDoubleSided to tell the two apart).
     */
    [[nodiscard]] bool isDoubleSided() const noexcept
    {
        return double_sided_.value_or(false);
    }

    /**
     * @brief States whether both sides of the surface are visible.
     *
     * @param double_sided true = both sides (no face culling by default), false = the source's single-sided surface.
     */
    void setDoubleSided(bool double_sided) noexcept
    {
        double_sided_ = double_sided;
    }

    /**
     * @brief Clears the two-sided statement, so the material is back to "the source stated nothing".
     */
    void clearDoubleSided() noexcept
    {
        double_sided_.reset();
    }

  protected:
    /// Name of the material; empty when unnamed.
    String name_;

    /// Concrete material category; assigned by derived class constructors.
    MaterialType material_type_ = MaterialType::Unknown;

    /// The source's two-sided statement; unset = the source stated nothing (see hasDoubleSided).
    std::optional<bool> double_sided_;
};

VN_MODELS_NS_END
