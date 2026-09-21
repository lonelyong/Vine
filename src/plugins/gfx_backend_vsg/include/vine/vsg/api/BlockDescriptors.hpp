#pragma once

#include <cstdint>
#include <memory>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/BindDescriptorSet.h>
#include <vsg/state/DescriptorSet.h>
#include <vsg/state/DescriptorSetLayout.h>
#include <vsg/state/PipelineLayout.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The descriptor side of the block storage: ONE set whose dynamic offsets select every block.
 *
 * WHY ONE SET. The alternative is a descriptor set per drawable (or per material), which means the frame path
 * allocates, writes and retires descriptor sets as content changes - and the two failure families this
 * backend has actually shipped are of exactly that kind (a resize that rebuilt every program slot, a material
 * edit that rebound descriptors). Binding the whole buffer ONCE, with `range` = one block and the block
 * chosen by a DYNAMIC OFFSET at draw time, is what makes "an edit is a write, a new frame is an offset" true
 * instead of aspirational.
 *
 * WHAT IT REFUSES. A dynamic offset must be a multiple of the device's `minUniformBufferOffsetAlignment`
 * (VUID-vkCmdBindDescriptorSets-pDynamicOffsets-01971). A misaligned offset is not a draw that comes out
 * slightly wrong: it is a validation error and, with validation off, undefined. So a bind with a misaligned
 * offset is REFUSED here - the caller gets no command, and the refusal is counted - instead of being handed
 * to the driver.
 *
 * REPOINTING. A session that replaced its storage has new bytes at the same layout, so the set's elements
 * are replaced while the LAYOUT survives: the old set stays alive through the commands that bind it (they
 * hold a reference), and the caller parks those commands as usual. Nothing here owns a frame's lifetime.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

/** @brief The block set: one dynamic uniform binding per block region. */
class BlockDescriptors
{
  public:
    /** @brief Binding of the per-pass view block. */
    static constexpr std::uint32_t kViewBinding = 0U;
    /** @brief Binding of the per-draw block. */
    static constexpr std::uint32_t kDrawBinding = 1U;
    /** @brief Binding of the material block. */
    static constexpr std::uint32_t kMaterialBinding = 2U;

    /** @brief Where this draw's three blocks are (the dynamic offsets, in binding order). */
    struct Offsets
    {
        std::uint64_t view{0};      ///< Offset of the view block (see `BlockStorage::writeView`).
        std::uint64_t draw{0};      ///< Offset of the draw block (see `BlockStorage::writeDraw`).
        std::uint64_t material{0};  ///< Offset of the material block (see `BlockStorage::writeMaterial`).
    };

  public:
    /** @brief Creates the layout and its set over a storage.
     *
     * @param device    The device the set and its pool belong to (the descriptors do not own it).
     * @param storage   The storage whose buffer is bound (its strides become the bindings' ranges).
     * @param set_index Index of the block set in the pipeline layouts that use it.
     * @return The descriptors, or null when the layout or the set could not be created.
     */
    static std::unique_ptr<BlockDescriptors> create(::vsg::ref_ptr<::vsg::Device> device,
                                                    const BlockStorage& storage, std::uint32_t set_index = 0U);

    ~BlockDescriptors();

    BlockDescriptors(const BlockDescriptors&) = delete;
    BlockDescriptors& operator=(const BlockDescriptors&) = delete;

  public:
    /** @brief Replaces the set with one over a new storage, keeping the layout.
     *
     * @param storage The storage that is bound from now on.
     * @return true when a new set was built; false when the storage has no buffer.
     */
    bool repoint(const BlockStorage& storage);

    /** @brief Builds the command that binds this set with three dynamic offsets.
     *
     * @param pipeline_layout The pipeline layout whose set at @ref setIndex is this set.
     * @param offsets         Where this draw's blocks are.
     * @return The command, or null when an offset is misaligned or does not fit the API's 32-bit field.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::BindDescriptorSet> bind(
        const ::vsg::ref_ptr<::vsg::PipelineLayout>& pipeline_layout, const Offsets& offsets);

  public:
    /** @brief Gets the set layout (shared by every pipeline that binds the block set). */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::DescriptorSetLayout> layout() const noexcept;

    /** @brief Gets the current set. */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::DescriptorSet> set() const noexcept;

    /** @brief Gets the index of the block set in the pipeline layout. */
    [[nodiscard]] std::uint32_t setIndex() const noexcept;

    /** @brief Gets the alignment every dynamic offset must satisfy on this device. */
    [[nodiscard]] std::uint64_t alignment() const noexcept;

    /** @brief Gets the number of binds refused (misaligned or out-of-range offsets). */
    [[nodiscard]] std::uint64_t refusals() const noexcept;

  private:
    struct Data;
    // Lexically after Data so the out-of-line destructor is the only place that needs the complete type.
    std::unique_ptr<Data> d;

    BlockDescriptors(::vsg::ref_ptr<::vsg::Device> device, const BlockStorage& storage, std::uint32_t set_index);
};

V_VSG_NS_END
