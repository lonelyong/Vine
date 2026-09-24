#pragma once

#include "geometry_global.hpp"

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
 */
class VN_GEOMETRY_API Material : public vn::Object, public vn::RefCounted<Material> {
    VN_OBJECT_META_DECL;

  public:
    Material();

  public:
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
    /// Concrete material category; assigned by derived class constructors.
    MaterialType material_type_ = MaterialType::Unknown;
};

VN_GEOMETRY_NS_END
