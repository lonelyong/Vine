#pragma once

#include "geometry_global.hpp"

#include "Primitive.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A cone primitive centered on the Z axis.
 */
class VN_GEOMETRY_API Cone : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit cone (radius 0.5, height 1.0).
     */
    Cone();

    /**
     * @brief Constructs a cone from its dimensions.
     *
     * @param radius Radius of the base circle.
     * @param height Extent along the Z axis.
     */
    Cone(double radius, double height);

  public:
    /**
     * @brief Returns the radius of the base circle.
     *
     * @return Radius.
     */
    double radius() const;

    /**
     * @brief Sets the radius of the base circle.
     *
     * @param radius New radius.
     */
    void setRadius(double radius);

    /**
     * @brief Returns the extent along the Z axis.
     *
     * @return Height.
     */
    double height() const;

    /**
     * @brief Sets the extent along the Z axis.
     *
     * @param height New height.
     */
    void setHeight(double height);

    [[nodiscard]]
    bool isValid() const override;

    /**
     * @brief Returns whether the cone encloses a volume.
     *
     * @param eps Numerical tolerance for the dimensions.
     * @return true when both radius and height are larger than eps.
     */
    [[nodiscard]]
    bool hasVolume(double eps = vn::math::EPS<double>()) const override;

  private:
    /// Radius of the base circle.
    double radius_ = 0.5;
    /// Extent along the Z axis.
    double height_ = 1.0;
};

VN_GEOMETRY_NS_END
