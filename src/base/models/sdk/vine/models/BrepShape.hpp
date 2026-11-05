#pragma once

#include <vine/models/models_global.hpp>

#include <memory>

#include <vine/models/Shape.hpp>

class TopoDS_Shape;

VN_MODELS_NS_BEGIN

/**
 * @brief A boundary-representation (B-rep) solid, holding the backend shape it was built from.
 *
 * @details
 * - The OpenCASCADE shape is held through a shared handle, so the solid OWNS what a loader produced: copying the
 *   BrepShape shares the shape and the last holder releases it. The SDK still only forward-declares the type, so it
 *   needs no OpenCASCADE header and no link to it.
 * - `shape()` lends the pointer out; it stays valid as long as this BrepShape (or another holder of the same shape)
 *   lives.
 * - `hasVolume()` answers false: whether the backend shape is closed is not queried here, so a solid reports itself
 *   as a surface rather than as a volume.
 */
class VN_MODELS_API BrepShape : public Shape {
    VN_OBJECT_META_DECL;

  public:
    BrepShape();
    ~BrepShape() override;

  public:
    /**
     * @brief Returns the underlying shape.
     *
     * @return The borrowed pointer, or null when no shape was set.
     */
    [[nodiscard]]
    const TopoDS_Shape* shape() const noexcept;

    /**
     * @brief Takes ownership of a shape.
     *
     * @param shape The shape to hold; null clears it.
     */
    void setShape(std::shared_ptr<const TopoDS_Shape> shape);

    [[nodiscard]]
    bool isValid() const override;

  private:
    /// The shape this solid holds; null until setShape() is given one.
    std::shared_ptr<const TopoDS_Shape> shape_;
};

VN_MODELS_NS_END
