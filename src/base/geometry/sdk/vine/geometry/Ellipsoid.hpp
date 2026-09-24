#pragma once

#include "geometry_global.hpp"

#include "Primitive.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief An axis-aligned ellipsoid primitive centered at the origin.
 */
class VN_GEOMETRY_API Ellipsoid : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit ellipsoid (all radii 0.5).
     */
    Ellipsoid();

    /**
     * @brief Constructs an ellipsoid from its semi-axis lengths.
     *
     * @param radius_x Semi-axis length along X.
     * @param radius_y Semi-axis length along Y.
     * @param radius_z Semi-axis length along Z.
     */
    Ellipsoid(double radius_x, double radius_y, double radius_z);

  public:
    /**
     * @brief Returns the semi-axis length along X.
     *
     * @return Radius along X.
     */
    double radiusX() const;

    /**
     * @brief Sets the semi-axis length along X.
     *
     * @param radius New radius along X.
     */
    void setRadiusX(double radius);

    /**
     * @brief Returns the semi-axis length along Y.
     *
     * @return Radius along Y.
     */
    double radiusY() const;

    /**
     * @brief Sets the semi-axis length along Y.
     *
     * @param radius New radius along Y.
     */
    void setRadiusY(double radius);

    /**
     * @brief Returns the semi-axis length along Z.
     *
     * @return Radius along Z.
     */
    double radiusZ() const;

    /**
     * @brief Sets the semi-axis length along Z.
     *
     * @param radius New radius along Z.
     */
    void setRadiusZ(double radius);

    [[nodiscard]]
    bool isValid() const override;

    /**
     * @brief Returns whether the ellipsoid encloses a volume.
     *
     * @param eps Numerical tolerance for the semi-axis lengths.
     * @return true when all three semi-axis lengths are larger than eps.
     */
    [[nodiscard]]
    bool hasVolume(double eps = vn::math::EPS<double>()) const override;

  private:
    /// Semi-axis length along X.
    double radius_x_ = 0.5;
    /// Semi-axis length along Y.
    double radius_y_ = 0.5;
    /// Semi-axis length along Z.
    double radius_z_ = 0.5;
};

VN_GEOMETRY_NS_END
