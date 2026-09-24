#pragma once

#include "geometry_global.hpp"

#include <vine/Colorf.hpp>

#include "Material.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A Phong shading material with ambient/diffuse/specular components.
 */
class VN_GEOMETRY_API PhongMaterial : public Material {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a default gray Phong material.
     */
    PhongMaterial();

    /**
     * @brief Constructs a material from all components.
     *
     * @param ambient   Ambient reflectance (linear RGBA).
     * @param diffuse   Diffuse reflectance (linear RGBA).
     * @param specular  Specular reflectance (linear RGBA).
     * @param shininess Specular exponent; larger values give sharper highlights.
     */
    PhongMaterial(const vn::Colorf& ambient, const vn::Colorf& diffuse,
        const vn::Colorf& specular, float shininess = 32.0f);

  public:
    /**
     * @brief Returns the ambient reflectance.
     *
     * @return Ambient color.
     */
    const vn::Colorf& ambient() const;

    /**
     * @brief Sets the ambient reflectance.
     *
     * @param ambient New ambient color.
     */
    void setAmbient(const vn::Colorf& ambient);

    /**
     * @brief Returns the diffuse reflectance.
     *
     * @return Diffuse color.
     */
    const vn::Colorf& diffuse() const;

    /**
     * @brief Sets the diffuse reflectance.
     *
     * @param diffuse New diffuse color.
     */
    void setDiffuse(const vn::Colorf& diffuse);

    /**
     * @brief Returns the specular reflectance.
     *
     * @return Specular color.
     */
    const vn::Colorf& specular() const;

    /**
     * @brief Sets the specular reflectance.
     *
     * @param specular New specular color.
     */
    void setSpecular(const vn::Colorf& specular);

    /**
     * @brief Returns the specular exponent.
     *
     * @return Shininess value.
     */
    float shininess() const;

    /**
     * @brief Sets the specular exponent.
     *
     * @param shininess New value.
     */
    void setShininess(float shininess);

    [[nodiscard]]
    const char* typeName() const override;

  private:
    /// Ambient reflectance.
    vn::Colorf ambient_{ 0.2f, 0.2f, 0.2f, 1.0f };
    /// Diffuse reflectance.
    vn::Colorf diffuse_{ 0.8f, 0.8f, 0.8f, 1.0f };
    /// Specular reflectance.
    vn::Colorf specular_{ 1.0f, 1.0f, 1.0f, 1.0f };
    /// Specular exponent.
    float shininess_ = 32.0f;
};

VN_GEOMETRY_NS_END
