#pragma once

#include "geometry_global.hpp"

#include <vine/Colorf.hpp>

#include "Material.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A simple material carrying a single base color.
 */
class VN_GEOMETRY_API ColorMaterial : public Material {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a white material.
     */
    ColorMaterial();

    /**
     * @brief Constructs a material from a color.
     *
     * @param color Base color (linear RGBA, 0..1).
     */
    explicit ColorMaterial(const vn::Colorf& color);

  public:
    /**
     * @brief Returns the base color.
     *
     * @return The material color.
     */
    const vn::Colorf& color() const;

    /**
     * @brief Sets the base color.
     *
     * @param color New color.
     */
    void setColor(const vn::Colorf& color);

    [[nodiscard]]
    const char* typeName() const override;

  private:
    /// Base color.
    vn::Colorf color_{ 1.0f, 1.0f, 1.0f, 1.0f };
};

VN_GEOMETRY_NS_END
