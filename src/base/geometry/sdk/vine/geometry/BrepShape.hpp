#pragma once

#include "geometry_global.hpp"

#include "Shape.hpp"

class TopoDS_Shape;

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A boundary-representation (B-rep) solid backed by an OpenCASCADE shape.
 *
 * The underlying TopoDS_Shape is stored as a non-owning pointer so the SDK
 * headers stay free of OpenCASCADE includes; ownership remains external.
 */
class VN_GEOMETRY_API BrepShape : public Shape {
    VN_OBJECT_META_DECL;

  public:
    BrepShape();
    ~BrepShape() override;

  public:
    /**
     * @brief Returns the underlying OCC shape.
     *
     * @return Pointer to the TopoDS_Shape, or null when none is set.
     */
    [[nodiscard]]
    const TopoDS_Shape* shape() const;

    /**
     * @brief Sets the underlying OCC shape (non-owning).
     *
     * @param shape Pointer to a TopoDS_Shape; the caller keeps ownership.
     */
    void setShape(TopoDS_Shape* shape);

    [[nodiscard]]
    bool isValid() const override;

  private:
    /// Underlying OCC shape; non-owning, may be null.
    TopoDS_Shape* shape_ = nullptr;
};

VN_GEOMETRY_NS_END