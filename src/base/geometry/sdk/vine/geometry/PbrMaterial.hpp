#pragma once

#include "geometry_global.hpp"

#include <vine/Colorf.hpp>

#include "Material.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A physically based (PBR) material with metallic/roughness shading.
 */
class VN_GEOMETRY_API PbrMaterial : public Material {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a default PBR material (white, non-metal, rough 0.5).
     */
    PbrMaterial();

    /**
     * @brief Constructs a PBR material from its core parameters.
     *
     * @param base_color Base (albedo) color.
     * @param metallic   Metallic factor in [0, 1].
     * @param roughness  Roughness factor in [0, 1].
     * @param opacity    Opacity factor; 1.0 is fully opaque.
     */
    PbrMaterial(const vn::Colorf& base_color, float metallic, float roughness, float opacity = 1.0f);

  public:
    /**
     * @brief Returns the base (albedo) color.
     *
     * @return Base color.
     */
    const vn::Colorf& baseColor() const;

    /**
     * @brief Sets the base (albedo) color.
     *
     * @param color New base color.
     */
    void setBaseColor(const vn::Colorf& color);

    /**
     * @brief Returns the emissive color.
     *
     * @return Emissive color.
     */
    const vn::Colorf& emissive() const;

    /**
     * @brief Sets the emissive color.
     *
     * @param color New emissive color.
     */
    void setEmissive(const vn::Colorf& color);

    /**
     * @brief Returns the metallic factor.
     *
     * @return Metallic in [0, 1].
     */
    float metallic() const;

    /**
     * @brief Sets the metallic factor.
     *
     * @param metallic New value in [0, 1].
     */
    void setMetallic(float metallic);

    /**
     * @brief Returns the roughness factor.
     *
     * @return Roughness in [0, 1].
     */
    float roughness() const;

    /**
     * @brief Sets the roughness factor.
     *
     * @param roughness New value in [0, 1].
     */
    void setRoughness(float roughness);

    /**
     * @brief Returns the opacity factor.
     *
     * @return Opacity in [0, 1]; 1.0 is fully opaque.
     */
    float opacity() const;

    /**
     * @brief Sets the opacity factor.
     *
     * @param opacity New value in [0, 1].
     */
    void setOpacity(float opacity);

    [[nodiscard]]
    const char* typeName() const override;

  private:
    /// Base (albedo) color.
    vn::Colorf base_color_{ 1.0f, 1.0f, 1.0f, 1.0f };
    /// Emissive color.
    vn::Colorf emissive_{ 0.0f, 0.0f, 0.0f, 1.0f };
    /// Metallic factor in [0, 1].
    float metallic_ = 0.0f;
    /// Roughness factor in [0, 1].
    float roughness_ = 0.5f;
    /// Opacity factor in [0, 1].
    float opacity_ = 1.0f;
};

VN_GEOMETRY_NS_END
