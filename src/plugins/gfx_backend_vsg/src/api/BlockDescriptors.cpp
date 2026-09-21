#include <vine/vsg/api/BlockDescriptors.hpp>

#include <array>
#include <cstdint>
#include <utility>

#include <vsg/state/BufferInfo.h>
#include <vsg/state/DescriptorBuffer.h>

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

}  // namespace

struct BlockDescriptors::Data
{
    Data(::vsg::ref_ptr<::vsg::Device> device_in, std::uint32_t set_index_in)
      : device(std::move(device_in)), set_index(set_index_in), alignment(uniformAlignment(device))
    {
    }

    /** @brief Builds the layout (once) with one dynamic uniform binding per block region. */
    bool makeLayout()
    {
        layout = ::vsg::DescriptorSetLayout::create();
        if (layout == nullptr) {
            return false;
        }
        layout->addBinding(kViewBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1U, kBlockStages);
        layout->addBinding(kDrawBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1U, kBlockStages);
        layout->addBinding(kMaterialBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1U, kBlockStages);
        return true;
    }

    /** @brief Builds a set over @p storage's buffer, one binding per region. */
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
        const auto descriptorFor = [buffer](std::uint32_t binding, std::uint64_t range) {
            auto buffer_info = ::vsg::BufferInfo::create(buffer, 0, range);
            return ::vsg::DescriptorBuffer::create(::vsg::BufferInfoList{ buffer_info }, binding, 0U,
                                                   VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
        };

        return ::vsg::DescriptorSet::create(layout, ::vsg::Descriptors{
                                                         descriptorFor(kViewBinding, strides.view),
                                                         descriptorFor(kDrawBinding, strides.draw),
                                                         descriptorFor(kMaterialBinding, strides.material),
                                                     });
    }

    /** @brief Whether one dynamic offset may be handed to the driver at all. */
    [[nodiscard]] bool usableOffset(std::uint64_t offset) const noexcept
    {
        return offset % alignment == 0 && offset <= 0xFFFFFFFFULL;
    }

    ::vsg::ref_ptr<::vsg::Device>              device;
    ::vsg::ref_ptr<::vsg::DescriptorSetLayout> layout;
    ::vsg::ref_ptr<::vsg::DescriptorSet>       set;
    std::uint32_t                              set_index{0};
    std::uint64_t                              alignment{kFallbackAlignment};
    std::uint64_t                              refusals{0};
};

BlockDescriptors::BlockDescriptors(::vsg::ref_ptr<::vsg::Device> device, const BlockStorage& storage,
                                   std::uint32_t set_index)
{
    // The alignment is read BEFORE the device is moved into the data (argument evaluation order is not a
    // promise), the same care BlockStorage takes.
    const std::uint64_t alignment = uniformAlignment(device);
    d                             = std::make_unique<Data>(std::move(device), set_index);
    d->alignment                  = alignment;
}

std::unique_ptr<BlockDescriptors> BlockDescriptors::create(::vsg::ref_ptr<::vsg::Device> device,
                                                           const BlockStorage& storage, std::uint32_t set_index)
{
    if (device == nullptr) {
        return nullptr;
    }
    auto descriptors = std::unique_ptr<BlockDescriptors>(new BlockDescriptors(std::move(device), storage, set_index));
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
    if (!d->usableOffset(offsets.view) || !d->usableOffset(offsets.draw) || !d->usableOffset(offsets.material)) {
        ++d->refusals;
        return {};
    }

    auto command = ::vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, d->set_index,
                                                    d->set);
    if (command == nullptr) {
        return {};
    }
    // One dynamic offset per binding, in BINDING order (view, draw, material) - the order the API consumes
    // them in, not the order they were written in.
    command->dynamicOffsets = { static_cast<std::uint32_t>(offsets.view),
                                static_cast<std::uint32_t>(offsets.draw),
                                static_cast<std::uint32_t>(offsets.material) };
    return command;
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

std::uint64_t BlockDescriptors::alignment() const noexcept
{
    return d->alignment;
}

std::uint64_t BlockDescriptors::refusals() const noexcept
{
    return d->refusals;
}

V_VSG_NS_END
