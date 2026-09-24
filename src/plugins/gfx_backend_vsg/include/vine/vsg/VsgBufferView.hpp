#pragma once

#include <vine/vsg/vsg_global.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

#include <vsg/core/Data.h>
#include <vsg/core/Inherit.h>
#include <vsg/core/Object.h>

#include <vine/Buffer.hpp>
#include <vine/intrusive_ptr.hpp>

VN_VSG_NS_BEGIN

namespace detail
{

/**
 * @brief A vsg Data that holds a Vine buffer, so a REAL vsg array can alias its memory.
 *
 * WHY IT EXISTS. The bridge used to materialise every channel into a vsg typed array (`vec3Array`, ...), and
 * the scene graph keeps that array referenced for the life of the node — so the vertex data sat in memory
 * twice: once in the model, once in the renderer. When a channel's layout already IS what the binding reads
 * there is nothing to materialise, and this class supplies the storage half of that.
 *
 * WHY IT IS NOT THE BOUND OBJECT. vsg's binding path is built around its OWN array types: a `vsg::Data`
 * subclass that merely reports matching `properties` and a valid `dataPointer()` does NOT bind. Measured: it
 * drew nothing while validation stayed clean, with `format` / `stride` / `valueSize` / `valueCount` /
 * `dataSize` all identical to a `vec3Array`'s. So the object the binding reads is a real `vsg::vec3Array` /
 * `vec2Array` / ... created over this storage with `vsg::Array(storage, offset, stride, count)`, which keeps
 * a ref_ptr to it. Because the storage holds the Vine buffer, the model may die first: the renderer keeps
 * exactly the memory it reads alive, and for no longer.
 *
 * WHAT THAT BUYS. The element type stays the ARRAY's, so its Vulkan format and stride keep being inferred
 * from it exactly as before — nothing about the binding is hand-written (see the note in VsgSceneRules.hpp
 * about a mismatched format being accepted silently).
 *
 * @tparam Element Scalar element type of the buffer.
 */
template <typename Element>
class VsgBufferView : public ::vsg::Inherit<::vsg::Data, VsgBufferView<Element>>
{
  public:
    /**
     * @brief Creates a view over @p buffer.
     *
     * @param buffer Buffer to hold and expose; null yields an empty view.
     */
    explicit VsgBufferView(intrusive_ptr<const vn::Buffer<Element>> buffer)
      : buffer_(std::move(buffer))
    {
        // `properties` lives in the dependent base `vsg::Data`, so it needs this-> (two-phase lookup).
        this->properties.stride = sizeof(Element);
    }

  public:
    VsgBufferView(const VsgBufferView&)            = delete;
    VsgBufferView& operator=(const VsgBufferView&) = delete;

  public:
    std::size_t valueSize() const override { return sizeof(Element); }

    std::size_t valueCount() const override { return buffer_ != nullptr ? buffer_->size() : 0u; }

    bool dataAvailable() const override { return buffer_ != nullptr && !buffer_->empty(); }

    std::size_t dataSize() const override { return valueCount() * sizeof(Element); }

    void* dataPointer() override { return const_cast<Element*>(elements()); }

    const void* dataPointer() const override { return elements(); }

    void* dataPointer(std::size_t index) override { return const_cast<Element*>(elements() + index); }

    const void* dataPointer(std::size_t index) const override { return elements() + index; }

    void* dataRelease() override { return nullptr; }

    std::uint32_t dimensions() const override { return 1u; }

    std::uint32_t width() const override { return static_cast<std::uint32_t>(valueCount()); }

    std::uint32_t height() const override { return 1u; }

    std::uint32_t depth() const override { return 1u; }

    ::vsg::ref_ptr<::vsg::Object> clone(const ::vsg::CopyOp&) const override
    {
        // A clone shares the buffer: copying it would reintroduce exactly the duplicate this exists to avoid.
        return VsgBufferView::create(buffer_);
    }

  private:
    const Element* elements() const { return buffer_ != nullptr ? buffer_->data() : nullptr; }

  private:
    intrusive_ptr<const vn::Buffer<Element>> buffer_;
};

} // namespace detail

VN_VSG_NS_END
