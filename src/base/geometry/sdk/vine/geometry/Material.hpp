#pragma once

#include "geometry_global.hpp"

#include <vine/INameable.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>

VN_GEOMETRY_NS_BEGIN

/**
 * @brief Concrete material category.
 */
enum class MaterialType {
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
class VN_GEOMETRY_API Material : public vn::Object, public vn::RefCounted<Material>, public vn::INameable {
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

  protected:
    /// Name of the material; empty when unnamed.
    String name_;

    /// Concrete material category; assigned by derived class constructors.
    MaterialType material_type_ = MaterialType::Unknown;
};

VN_GEOMETRY_NS_END
