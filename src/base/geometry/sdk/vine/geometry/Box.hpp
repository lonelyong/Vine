#pragma once

#include "geometry_global.hpp"

#include <vine/math/Types.hpp>

#include "Primitive.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief An axis-aligned box primitive centered at the origin, sized in the shape's local space.
 */
class VN_GEOMETRY_API Box : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit box (1 x 1 x 1).
     */
    Box()
    {
        shape_type_ = ShapeType::Box;
    }

    /**
     * @brief Constructs a box from edge lengths.
     *
     * @param width  Size along the X axis.
     * @param height Size along the Y axis.
     * @param depth  Size along the Z axis.
     */
    Box(float width, float height, float depth)
      : width_(width)
      , height_(height)
      , depth_(depth)
    {
        shape_type_ = ShapeType::Box;
    }

    /**
     * @brief Returns the size along the X axis.
     *
     * @return Width.
     */
    float width() const
    {
        return width_;
    }

    /**
     * @brief Sets the size along the X axis.
     *
     * @param width New width.
     */
    void setWidth(float width)
    {
        width_ = width;
    }

    /**
     * @brief Returns the size along the Y axis.
     *
     * @return Height.
     */
    float height() const
    {
        return height_;
    }

    /**
     * @brief Sets the size along the Y axis.
     *
     * @param height New height.
     */
    void setHeight(float height)
    {
        height_ = height;
    }

    /**
     * @brief Returns the size along the Z axis.
     *
     * @return Depth.
     */
    float depth() const
    {
        return depth_;
    }

    /**
     * @brief Sets the size along the Z axis.
     *
     * @param depth New depth.
     */
    void setDepth(float depth)
    {
        depth_ = depth;
    }

    [[nodiscard]]
    bool isValid() const override
    {
        return width_ > 0.0f && height_ > 0.0f && depth_ > 0.0f;
    }

    /**
     * @brief Returns whether the box encloses a volume.
     *
     * @param eps Numerical tolerance for the edge lengths.
     * @return true when all three edge lengths are larger than eps.
     */
    [[nodiscard]]
    bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override
    {
        const float tolerance = static_cast<float>(eps);
        return width_ > tolerance && height_ > tolerance && depth_ > tolerance;
    }

  private:
    /// Size along the X axis.
    float width_ = 1.0f;
    /// Size along the Y axis.
    float height_ = 1.0f;
    /// Size along the Z axis.
    float depth_ = 1.0f;
};

VN_GEOMETRY_NS_END
