#pragma once

#include "geometry_global.hpp"

#include "Primitive.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief An axis-aligned box primitive centered at the origin.
 */
class VN_GEOMETRY_API Box : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit box (1 x 1 x 1).
     */
    Box();

    /**
     * @brief Constructs a box from edge lengths.
     *
     * @param width  Size along the X axis.
     * @param height Size along the Y axis.
     * @param depth  Size along the Z axis.
     */
    Box(double width, double height, double depth);

  public:
    /**
     * @brief Returns the size along the X axis.
     *
     * @return Width.
     */
    double width() const;

    /**
     * @brief Sets the size along the X axis.
     *
     * @param width New width.
     */
    void setWidth(double width);

    /**
     * @brief Returns the size along the Y axis.
     *
     * @return Height.
     */
    double height() const;

    /**
     * @brief Sets the size along the Y axis.
     *
     * @param height New height.
     */
    void setHeight(double height);

    /**
     * @brief Returns the size along the Z axis.
     *
     * @return Depth.
     */
    double depth() const;

    /**
     * @brief Sets the size along the Z axis.
     *
     * @param depth New depth.
     */
    void setDepth(double depth);

    [[nodiscard]]
    bool isValid() const override;

    /**
     * @brief Returns whether the box encloses a volume.
     *
     * @param eps Numerical tolerance for the edge lengths.
     * @return true when all three edge lengths are larger than eps.
     */
    [[nodiscard]]
    bool hasVolume(double eps = vn::math::EPS<double>()) const override;

  private:
    /// Size along the X axis.
    double width_  = 1.0;
    /// Size along the Y axis.
    double height_ = 1.0;
    /// Size along the Z axis.
    double depth_  = 1.0;
};

VN_GEOMETRY_NS_END
