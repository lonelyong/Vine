#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/BindDescriptorSet.h>
#include <vsg/state/DescriptorSet.h>
#include <vsg/state/DescriptorSetLayout.h>
#include <vsg/state/ImageView.h>
#include <vsg/state/PipelineLayout.h>
#include <vsg/state/Sampler.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ProgramAbi.hpp>
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
 * WHICH BLOCKS A SET CARRIES IS A SHAPE, NOT A CONSTANT. The canonical arrangement declares the five L1
 * blocks at bindings 0..4 of set 0, and that is what a session builds when it passes no shape at all. But a
 * block set belongs to ONE program: a program whose text declares its blocks elsewhere (the engine's own
 * programs declare the material at binding 0 and the per-drawable block in set 1, say) is served by building
 * the very shape its text declares - the same rule the pipeline layer follows, and the reason this class
 * takes a list of (binding, role) pairs rather than five named members. `layoutOfShape` is the ONE spelling
 * of "what that shape's set layout is": the layer compiles its pipelines against it and the set here is
 * built from it, so the two can never be separately defined.
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

/** @brief The block set: one dynamic uniform binding per block the shape declares. */
class BlockDescriptors
{
  public:
    /** @brief Binding of the per-pass view block (the canonical shape's spelling). */
    static constexpr std::uint32_t kViewBinding = 0U;
    /** @brief Binding of the per-draw block (the canonical shape's spelling). */
    static constexpr std::uint32_t kDrawBinding = 1U;
    /** @brief Binding of the material block (the canonical shape's spelling). */
    static constexpr std::uint32_t kMaterialBinding = 2U;
    /** @brief Binding of the forward light block (the canonical shape's spelling; see api/LightBlock.hpp). */
    static constexpr std::uint32_t kLightsBinding = 3U;
    /** @brief Binding of the shadow block (the canonical shape's spelling; see api/ShadowBlock.hpp). */
    static constexpr std::uint32_t kShadowBinding = 4U;

    /** @brief One binding a block set declares: which L1 block is read there. */
    struct Binding
    {
        std::uint32_t binding{0};                     ///< Binding index inside the set.
        AbiBlockRole  role{AbiBlockRole::NotABlock};  ///< The block role read at that binding.
    };

    /** @brief One sampled image a declared set binds (the ENGINE's set 0 carries the material block AND the
     *         diffuse map, so the two kinds share a set - `layoutOfShape` declares both). */
    struct SampledBinding
    {
        std::uint32_t                    binding{0};  ///< The binding the program declares the sampler at.
        ::vsg::ref_ptr<::vsg::ImageView> view;        ///< The image read there.
        ::vsg::ref_ptr<::vsg::Sampler>   sampler;     ///< Its sampler (a content layer's input sampler).
    };

    /** @brief Where this draw's blocks are (the dynamic offsets, named by role - NOT by binding index). */
    struct Offsets
    {
        std::uint64_t view{0};      ///< Offset of the view block (see `BlockStorage::writeView`).
        std::uint64_t draw{0};      ///< Offset of the draw block (see `BlockStorage::writeDraw`).
        std::uint64_t material{0};  ///< Offset of the material block (see `BlockStorage::writeMaterial`).
        std::uint64_t lights{0};    ///< Offset of the light block (see `BlockStorage::writeLights`).
        std::uint64_t shadow{0};    ///< Offset of the shadow block (see `BlockStorage::writeShadows`).
    };

  public:
    /** @brief Gets the canonical shape: the five L1 blocks at bindings 0..4 of a set.
     *
     * This is what a program that declares the blocks at the bindings this backend's own programs use gets,
     * and the shape a caller builds when it passes none. The values are the binding constants above, so the
     * shape and the names cannot drift apart.
     *
     * @return The shape, in binding order.
     */
    [[nodiscard]] static std::span<const Binding> canonicalShape() noexcept;

    /** @brief Gets the set layout a shape declares: one dynamic uniform binding per block entry and one
     *         combined image sampler per sampled entry, both in binding order.
     *
     * The ONE spelling of that layout: a pipeline is compiled against it and the set is built from it (see
     * the file note). Every binding is declared readable from the vertex and the fragment stage, because a
     * block's readers are the shader's business and the set a pass shares cannot be per-program.
     *
     * @param shape            The block bindings to declare (one per block role the shape carries).
     * @param sampler_bindings The sampled images' bindings (the ENGINE's set 0 carries the material block AND
     *                         the diffuse map, so one layout declares both kinds).
     * @return The layout, or null when it could not be created.
     */
    [[nodiscard]] static ::vsg::ref_ptr<::vsg::DescriptorSetLayout> layoutOfShape(
        std::span<const Binding> shape, std::span<const std::uint32_t> sampler_bindings = {});

    /** @brief Creates the layout and its canonical set over a storage.
     *
     * @param device    The device the set and its pool belong to (the descriptors do not own it).
     * @param storage   The storage whose buffer is bound (its strides become the bindings' ranges).
     * @param set_index Index of the block set in the pipeline layouts that use it.
     * @return The descriptors, or null when the layout or the set could not be created.
     */
    static std::unique_ptr<BlockDescriptors> create(::vsg::ref_ptr<::vsg::Device> device,
                                                    const BlockStorage& storage, std::uint32_t set_index = 0U);

    /** @brief Creates the layout and its set over a storage, for the shape @p shape declares.
     *
     * @param device    The device the set and its pool belong to (the descriptors do not own it).
     * @param storage   The storage whose buffer is bound (its strides become the bindings' ranges).
     * @param shape     The bindings the set declares (one per block role; see `layoutOfShape`).
     * @param set_index Index of the block set in the pipeline layouts that use it.
     * @param samplers  The sampled images the program declares in the SAME set (empty for a blocks-only set).
     * @return The descriptors, or null when the layout or the set could not be created.
     */
    static std::unique_ptr<BlockDescriptors> create(::vsg::ref_ptr<::vsg::Device> device,
                                                    const BlockStorage& storage, std::span<const Binding> shape,
                                                    std::uint32_t set_index,
                                                    std::span<const SampledBinding> samplers = {});

    /** @brief Creates the set @p abi's declared blocks are bound through, for one set index.
     *
     * The shape IS the program's own declaration (`blockShapeOf`), which is the whole point: a pipeline layer
     * built from the same facts compiled its pipelines against that layout, so the set a caller binds here
     * and the layout those pipelines carry cannot be separately defined. A program that declares no blocks in
     * @p set gets an EMPTY set (bindable, and read by nothing).
     *
     * @param abi       The program's declared bindings.
     * @param set       Descriptor set index.
     * @param device    The device the set and its pool belong to.
     * @param storage   The storage whose buffer is bound.
     * @return The descriptors, or null when the set could not be created.
     */
    static std::unique_ptr<BlockDescriptors> forAbi(const ProgramAbi& abi, std::uint32_t set,
                                                    ::vsg::ref_ptr<::vsg::Device> device,
                                                    const BlockStorage& storage,
                                                    std::span<const SampledBinding> samplers = {});

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

    /** @brief Builds the command that binds this set with one dynamic offset per declared binding.
     *
     * @param pipeline_layout The pipeline layout whose set at @ref setIndex is this set.
     * @param offsets         Where this draw's blocks are (picked BY ROLE, handed over in binding order).
     * @return The command, or null when an offset is misaligned or does not fit the API's 32-bit field.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::BindDescriptorSet> bind(
        const ::vsg::ref_ptr<::vsg::PipelineLayout>& pipeline_layout, const Offsets& offsets);

  public:
    /** @brief Gets the shape this set was built for (in binding order). */
    [[nodiscard]] std::span<const Binding> shape() const noexcept;

    /** @brief Gets the sampled images this set binds (empty for a blocks-only set). */
    [[nodiscard]] std::span<const SampledBinding> samplers() const noexcept;

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

    BlockDescriptors(::vsg::ref_ptr<::vsg::Device> device, const BlockStorage& storage,
                     std::span<const Binding> shape, std::uint32_t set_index,
                     std::span<const SampledBinding> samplers);
};

/** @brief Gets the block shape @p abi declares in @p set, in binding order.
 *
 * The ONE spelling of "which blocks a program puts in one of its sets": the pipeline layer builds its layouts
 * from it and a caller builds the very set those pipelines bind from it, so the two cannot drift apart.
 *
 * @param abi The program's declared bindings.
 * @param set The descriptor set index to read.
 * @return One entry per declared block, in binding order (empty when the text declares none there).
 */
[[nodiscard]] std::vector<BlockDescriptors::Binding> blockShapeOf(const ProgramAbi& abi, std::uint32_t set);

V_VSG_NS_END
