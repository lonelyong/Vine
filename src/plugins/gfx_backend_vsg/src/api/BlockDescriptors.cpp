#include <vine/vsg/api/BlockDescriptors.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <vsg/state/BufferInfo.h>
#include <vsg/state/DescriptorBuffer.h>
#include <vsg/state/DescriptorImage.h>
#include <vsg/state/ImageInfo.h>

V_VSG_NS_BEGIN

namespace
{

/// @brief Fallback when the device reports no uniform-buffer alignment (a null physical device).
constexpr std::uint64_t kFallbackAlignment = 256U;

/// @brief Gets the alignment a dynamic offset must honour on @p device.
std::uint64_t uniformAlignment(const ::vsg::ref_ptr<::vsg::Device>& device) noexcept
{
    if (auto physical = device != nullptr ? device->getPhysicalDevice() : nullptr) {
        const std::uint64_t reported = physical->getProperties().limits.minUniformBufferOffsetAlignment;
        if (reported != 0) {
            return reported;
        }
    }
    return kFallbackAlignment;
}

/// @brief The stages a block's values are read at (a matrix in the vertex stage, an opacity in the fragment).
constexpr VkShaderStageFlags kBlockStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

/// @brief The distance between two blocks of @p role in a storage (0U for a role that is not a block).
std::uint64_t strideOf(AbiBlockRole role, const BlockStorage::Strides& strides) noexcept
{
    switch (role)
    {
    case AbiBlockRole::View: return strides.view;
    case AbiBlockRole::Draw: return strides.draw;
    case AbiBlockRole::Material: return strides.material;
    case AbiBlockRole::Lights: return strides.light;
    case AbiBlockRole::ShadowBlock: return strides.shadow;
    case AbiBlockRole::NotABlock:
    case AbiBlockRole::Foreign: return 0U;
    }
    return 0U;
}

/// @brief The offset @p offsets has for @p role (the shape's bindings pick their own role's).
std::uint64_t offsetOf(const BlockDescriptors::Offsets& offsets, AbiBlockRole role) noexcept
{
    switch (role)
    {
    case AbiBlockRole::View: return offsets.view;
    case AbiBlockRole::Draw: return offsets.draw;
    case AbiBlockRole::Material: return offsets.material;
    case AbiBlockRole::Lights: return offsets.lights;
    case AbiBlockRole::ShadowBlock: return offsets.shadow;
    case AbiBlockRole::NotABlock:
    case AbiBlockRole::Foreign: return 0U;
    }
    return 0U;
}

}  // namespace

struct BlockDescriptors::Data
{
    Data(::vsg::ref_ptr<::vsg::Device> device_in, std::span<const Binding> shape_in, std::uint32_t set_index_in,
         std::span<const SampledBinding> samplers_in)
      : device(std::move(device_in)), shape(shape_in.begin(), shape_in.end()), samplers(samplers_in.begin(), samplers_in.end()),
        set_index(set_index_in), alignment(uniformAlignment(device))
    {
    }

    /** @brief Builds the layout (once): dynamic uniforms for the blocks and sampled images for the maps. */
    bool makeLayout()
    {
        std::vector<std::uint32_t> sampler_bindings;
        sampler_bindings.reserve(samplers.size());
        for (const SampledBinding& entry : samplers) {
            sampler_bindings.push_back(entry.binding);
        }
        layout = layoutOfShape(shape, sampler_bindings);
        return layout != nullptr;
    }

    /** @brief Builds a set over @p storage's buffer, one binding per entry of the shape. */
    ::vsg::ref_ptr<::vsg::DescriptorSet> makeSet(const BlockStorage& storage) const
    {
        auto* const buffer = storage.buffer().get();
        if (buffer == nullptr || layout == nullptr) {
            return {};
        }
        const BlockStorage::Strides strides = storage.strides();

        // `range` is ONE block and the descriptor's own offset stays 0: which block a draw reads is decided
        // by the dynamic offset at bind time, which is what lets every draw share this one set. The factory's
        // argument order is (buffer info, DESTINATION BINDING, array element, type) - passing the binding in
        // the array-element slot declares three descriptors at binding 0, which a set layout with one element
        // per binding then resolves as an out-of-range element rather than as the error it is.
        ::vsg::Descriptors descriptors;
        descriptors.reserve(shape.size() + samplers.size());
        for (const Binding& entry : shape) {
            auto buffer_info = ::vsg::BufferInfo::create(buffer, 0, strideOf(entry.role, strides));
            descriptors.push_back(::vsg::DescriptorBuffer::create(
                ::vsg::BufferInfoList{ buffer_info }, entry.binding, 0U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC));
        }
        // The images the program declares in the SAME set (the engine's set 0 carries the material block and
        // the diffuse map): bound at the binding the text names, with the sampler the caller supplies. The
        // image is left in SHADER_READ_ONLY: a set a pass binds reads its inputs, never writes them.
        for (const SampledBinding& entry : samplers) {
            if (entry.view == nullptr || entry.sampler == nullptr) {
                return {};
            }
            descriptors.push_back(::vsg::DescriptorImage::create(
                ::vsg::ImageInfoList{ ::vsg::ImageInfo::create(entry.sampler, entry.view,
                                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) },
                entry.binding, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
        }
        return ::vsg::DescriptorSet::create(layout, descriptors);
    }

    /** @brief Whether one dynamic offset may be handed to the driver at all. */
    [[nodiscard]] bool usableOffset(std::uint64_t offset) const noexcept
    {
        return offset % alignment == 0 && offset <= 0xFFFFFFFFULL;
    }

    ::vsg::ref_ptr<::vsg::Device>              device;
    std::vector<Binding>                       shape;
    std::vector<SampledBinding>                samplers;
    ::vsg::ref_ptr<::vsg::DescriptorSetLayout> layout;
    ::vsg::ref_ptr<::vsg::DescriptorSet>       set;
    std::uint32_t                              set_index{0};
    BlockDescriptors::ImageSource              source{};  ///< The texture revision the sampled images came from.
    std::uint64_t                              alignment{kFallbackAlignment};
    std::uint64_t                              refusals{0};
};

std::span<const BlockDescriptors::Binding> BlockDescriptors::canonicalShape() noexcept
{
    // The five L1 blocks at the bindings this backend's own programs declare them at (see
    // `.ai/design/vsg-reimplementation.md` §11.16ae): the ENGINE's programs declare them elsewhere, and a
    // caller that serves such a program builds that shape instead.
    static constexpr Binding kCanonical[] = {
        { kViewBinding, AbiBlockRole::View },        { kDrawBinding, AbiBlockRole::Draw },
        { kMaterialBinding, AbiBlockRole::Material }, { kLightsBinding, AbiBlockRole::Lights },
        { kShadowBinding, AbiBlockRole::ShadowBlock },
    };
    return kCanonical;
}

std::vector<BlockDescriptors::Binding> blockShapeOf(const ProgramAbi& abi, std::uint32_t set)
{
    std::vector<BlockDescriptors::Binding> shape;
    for (const AbiBinding& binding : abi.bindings)
    {
        if (binding.set == set && binding.kind == AbiDescriptorKind::UniformBlock)
        {
            shape.push_back(BlockDescriptors::Binding{ binding.binding, binding.role });
        }
    }
    return shape;
}

::vsg::ref_ptr<::vsg::DescriptorSetLayout> BlockDescriptors::layoutOfShape(
    std::span<const Binding> shape, std::span<const std::uint32_t> sampler_bindings)
{
    auto layout = ::vsg::DescriptorSetLayout::create();
    if (layout == nullptr) {
        return {};
    }
    for (const Binding& entry : shape) {
        layout->addBinding(entry.binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1U, kBlockStages);
    }
    for (const std::uint32_t binding : sampler_bindings) {
        layout->addBinding(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1U, kBlockStages);
    }
    return layout;
}

BlockDescriptors::BlockDescriptors(::vsg::ref_ptr<::vsg::Device> device, const BlockStorage& storage,
                                  std::span<const Binding> shape, std::uint32_t set_index,
                                  std::span<const SampledBinding> samplers, ImageSource source)
{
    // The alignment is read BEFORE the device is moved into the data (argument evaluation order is not a
    // promise), the same care BlockStorage takes.
    const std::uint64_t alignment = uniformAlignment(device);
    d                             = std::make_unique<Data>(std::move(device), shape, set_index, samplers);
    d->alignment                  = alignment;
    d->source                     = source;
    (void)storage;
}

std::unique_ptr<BlockDescriptors> BlockDescriptors::create(::vsg::ref_ptr<::vsg::Device> device,
                                                           const BlockStorage& storage, std::uint32_t set_index)
{
    return create(std::move(device), storage, canonicalShape(), set_index);
}

std::unique_ptr<BlockDescriptors> BlockDescriptors::forAbi(const ProgramAbi& abi, std::uint32_t set,
                                                          ::vsg::ref_ptr<::vsg::Device> device,
                                                          const BlockStorage& storage,
                                                          std::span<const SampledBinding> samplers,
                                                          ImageSource source)
{
    const std::vector<Binding> shape = blockShapeOf(abi, set);
    return create(std::move(device), storage, shape, set, samplers, source);
}

std::unique_ptr<BlockDescriptors> BlockDescriptors::create(::vsg::ref_ptr<::vsg::Device> device,
                                                           const BlockStorage& storage, std::span<const Binding> shape,
                                                           std::uint32_t set_index,
                                                           std::span<const SampledBinding> samplers,
                                                           ImageSource source)
{
    if (device == nullptr) {
        return nullptr;
    }
    auto descriptors = std::unique_ptr<BlockDescriptors>(new BlockDescriptors(std::move(device), storage, shape, set_index,
                                                                               samplers, source));
    if (!descriptors->d->makeLayout()) {
        return nullptr;
    }
    descriptors->d->set = descriptors->d->makeSet(storage);
    return descriptors->d->set != nullptr ? std::move(descriptors) : nullptr;
}

BlockDescriptors::~BlockDescriptors() = default;

bool BlockDescriptors::repoint(const BlockStorage& storage)
{
    ::vsg::ref_ptr<::vsg::DescriptorSet> replacement = d->makeSet(storage);
    if (replacement == nullptr) {
        return false;
    }
    // The layout survives; the OLD set does not die here unless nothing else holds it - the commands that
    // bound it hold their own reference, and a frame still recording is exactly why the caller parks them.
    d->set = std::move(replacement);
    return true;
}

::vsg::ref_ptr<::vsg::BindDescriptorSet> BlockDescriptors::bind(
    const ::vsg::ref_ptr<::vsg::PipelineLayout>& pipeline_layout, const Offsets& offsets)
{
    if (pipeline_layout == nullptr || d->set == nullptr) {
        return {};
    }
    // Refuse rather than hand the driver an offset the API forbids: a misaligned dynamic offset is a
    // validation error with validation on and undefined behaviour with it off.
    for (const Binding& entry : d->shape) {
        if (!d->usableOffset(offsetOf(offsets, entry.role))) {
            ++d->refusals;
            return {};
        }
    }

    auto command = ::vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, d->set_index,
                                                    d->set);
    if (command == nullptr) {
        return {};
    }
    // One dynamic offset per binding, in the SHAPE's binding order - the order the API consumes them in, not
    // the order they were written in. The offsets are named by role, so a set that carries, say, only the
    // material and the lights picks exactly those two and in its own binding order.
    command->dynamicOffsets.reserve(d->shape.size());
    for (const Binding& entry : d->shape) {
        command->dynamicOffsets.push_back(static_cast<std::uint32_t>(offsetOf(offsets, entry.role)));
    }
    return command;
}

std::span<const BlockDescriptors::Binding> BlockDescriptors::shape() const noexcept
{
    return d->shape;
}

std::span<const BlockDescriptors::SampledBinding> BlockDescriptors::samplers() const noexcept
{
    return d->samplers;
}

::vsg::ref_ptr<::vsg::DescriptorSetLayout> BlockDescriptors::layout() const noexcept
{
    return d->layout;
}

::vsg::ref_ptr<::vsg::DescriptorSet> BlockDescriptors::set() const noexcept
{
    return d->set;
}

std::uint32_t BlockDescriptors::setIndex() const noexcept
{
    return d->set_index;
}

BlockDescriptors::ImageSource BlockDescriptors::source() const noexcept
{
    return d->source;
}

std::uint64_t BlockDescriptors::alignment() const noexcept
{
    return d->alignment;
}

std::uint64_t BlockDescriptors::refusals() const noexcept
{
    return d->refusals;
}

V_VSG_NS_END
