#include <vine/backend_vk/internal/FrameScratch.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <utility>

#include <vine/backend_vk/internal/BarrierPlan.hpp>

#include <vine/graphics/DrawAbi.hpp>
#include <vine/graphics/LightAbi.hpp>
#include <vine/graphics/MaterialAbi.hpp>
#include <vine/graphics/ShaderAbi.hpp>

VN_BACKEND_VK_NS_BEGIN

struct FrameScratch::Impl {
    const VolkDeviceTable* table{ nullptr };
    VkDevice               device{ VK_NULL_HANDLE };
    /// The device's memory pool (borrowed: the device owns it and outlives this scratch).
    MemoryAllocator*     allocator{ nullptr };
    std::uint32_t        uniform_alignment{ detail::kDefaultUniformAlignment };
    /// The device's set layouts (borrowed: the caches own them and outlive this scratch).
    VkDescriptorSetLayout content_set_layout{ VK_NULL_HANDLE };
    VkDescriptorSetLayout draw_set_layout{ VK_NULL_HANDLE };
    VkDescriptorSetLayout fullscreen_set_layout{ VK_NULL_HANDLE };
    /// The pool the set cache allocates from: freed PER ENTRY, because a cached set outlives the frame that built it.
    VkDescriptorPool descriptor_pool{ VK_NULL_HANDLE };

    /**
     * @brief The frame's uniform constants: ONE host-visible buffer, slices handed out by byte offset.
     *
     * @details
     * - THE COST MODEL OF THIS PATH: a draw's constants are WRITTEN into a slice and bound with a DYNAMIC offset, so
     *   a steady frame creates no buffer and no memory. It used to create and destroy one of each per draw (28 of
     *   each in the demo's frame - `.ai/design/vk-backend.md` 2.4 records the measured before/after).
     * - THE RING IS REWRITTEN EVERY FRAME: the driver submits and waits inside executeFrame, so when beginFrame runs
     *   nothing the previous frame recorded is still reading it.
     * - A FRAME THAT DOES NOT FIT IS SKIPPED, NOT GROWN IN PLACE: growing would destroy a buffer the command buffer
     *   being recorded already names. `demand` remembers what the frame asked for and beginFrame grows to it before
     *   anything is recorded - the same "grow between frames" rule the reference backend's block storage states.
     * - REPLACING IT DROPS THE SET CACHE: every cached set names the old buffer (see ensureRing).
     */
    struct UniformRing {
        VkBuffer     buffer{ VK_NULL_HANDLE };
        MemorySlice  memory{};
        std::byte*   mapped{ nullptr };
        VkDeviceSize capacity{ 0 };
        VkDeviceSize cursor{ 0 };
        VkDeviceSize demand{ 0 };
    };
    UniformRing ring;

    /**
     * @brief A cached descriptor set: what it BINDS, and when it was last used.
     *
     * @details
     * - A SET IS A BINDING OF RESOURCES, NOT OF A DRAW: with the constants bound through dynamic offsets, a content
     *   set depends only on the five texture VIEWS, the five texture SAMPLERS they are read with, and the shadow map's
     *   view and sampler; a full-screen set on the source's views and the same shadow pair. Both are stable across draws AND
     *   frames, which is what makes them cacheable at all (F2 of the review: they used to be allocated per draw, per
     *   frame).
     * - THE SAMPLERS ARE PART OF THE KEY because they are part of the VALUE. A `VkDescriptorImageInfo` names a view AND
     *   a sampler (`vkUpdateDescriptorSets` writes a combined image sampler), and two materials may read ONE texture
     *   through two authored samplers (`Material::setTexture(slot, texture, sampler)`): a key that stopped at the view
     *   served the first material's sampler to the second, which is silent wrongness rather than a cache miss (`G4`:
     *   a key must name everything the cached value depends on).
     * - `fullscreen` is part of the key because the two layouts are different objects even when they name the same views.
     */
    struct SetKey {
        bool                                            fullscreen{ false };
        std::array<VkImageView, detail::kTextureSlots>  views{};
        std::array<VkSampler, detail::kTextureSlots>    samplers{};
        VkImageView                                    shadow{ VK_NULL_HANDLE };
        VkSampler                                      shadow_sampler{ VK_NULL_HANDLE };

        [[nodiscard]] bool operator<(const SetKey& other) const
        {
            if (fullscreen != other.fullscreen) {
                return fullscreen < other.fullscreen;
            }
            if (views != other.views) {
                return views < other.views;
            }
            if (samplers != other.samplers) {
                return samplers < other.samplers;
            }
            if (shadow != other.shadow) {
                return shadow < other.shadow;
            }
            return shadow_sampler < other.shadow_sampler;
        }
    };
    struct SetEntry {
        VkDescriptorSet set{ VK_NULL_HANDLE };
        std::uint64_t   last_frame{ 0 };
    };
    std::map<SetKey, SetEntry> sets;
    /// The per-draw set: its only binding is the ring's draw block, so ONE handle serves every draw of a generation.
    VkDescriptorSet draw_set{ VK_NULL_HANDLE };
    /// Frames this scratch has begun - the stamp a cached set is evicted by.
    std::uint64_t frame_index{ 0 };

    std::uint64_t ring_builds_{ 0 };
    std::uint64_t set_builds_{ 0 };

    /** @brief Byte offset of the light block inside one draw's uniform buffer (material starts at 0). */
    [[nodiscard]] VkDeviceSize lightsOffset() const noexcept
    {
        return detail::alignedUp(sizeof(vn::graphics::VineMaterialBlock), uniform_alignment);
    }

    /** @brief Byte offset of the draw block inside one draw's uniform buffer. */
    [[nodiscard]] VkDeviceSize drawOffset() const noexcept
    {
        return detail::alignedUp(lightsOffset() + sizeof(vn::graphics::VineLightsBlock), uniform_alignment);
    }

    /** @brief Byte offset of the shadow block inside one draw's uniform buffer. */
    [[nodiscard]] VkDeviceSize shadowOffset() const noexcept
    {
        return detail::alignedUp(drawOffset() + sizeof(vn::graphics::VineDrawBlock), uniform_alignment);
    }

    /** @brief Size of one draw's uniform buffer: material, lights, draw and the (unused-for-now) shadow block. */
    [[nodiscard]] VkDeviceSize uniformSize() const noexcept
    {
        return shadowOffset() + sizeof(vn::graphics::VineShadowBlock);
    }

    [[nodiscard]] bool createHostBuffer(const VkDeviceSize size,
                                        const VkBufferUsageFlags usage,
                                        VkBuffer&                buffer,
                                        MemorySlice&             memory) const
    {
        return detail::createHostBuffer(*table, device, *allocator, size, usage, buffer, memory);
    }

    void destroyBuffer(VkBuffer& buffer, MemorySlice& memory) const noexcept
    {
        detail::destroyBuffer(*table, device, *allocator, buffer, memory);
    }

    /**
     * @brief Allocates one descriptor set from @p layout.
     *
     * @param layout The set layout the set must agree with.
     * @return The set, or `VK_NULL_HANDLE` when the pool is exhausted.
     */
    [[nodiscard]] VkDescriptorSet allocateSet(VkDescriptorSetLayout layout) const
    {
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool     = descriptor_pool;
        allocate.descriptorSetCount = 1U;
        allocate.pSetLayouts        = &layout;

        VkDescriptorSet set = VK_NULL_HANDLE;
        if (table->vkAllocateDescriptorSets(device, &allocate, &set) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        return set;
    }

    /**
     * @brief Writes the engine's shadow pair into @p set: the map at binding 7 and its block at binding 8.
     *
     * @details
     * - ONE SPELLING FOR BOTH SETS: a content set and a full-screen set state the same two bindings (the engine's
     *   programs declare them unconditionally), and spelling the pair twice is how the full-screen half once kept a
     *   white stand-in while the content half had the map (see .ai/bugs/vk-screen-pass-shaded-without-the-map.md).
     * - The CALLER decides which map: the pass' resolved one, or the white stand-in for "no shadow reaches this
     *   pass" (see buildShadowBlock).
     *
     * @param set   The set to write into (allocated from a layout that declares both bindings).
     * @param map   The map's image info: its sampler, view and the SHADER_READ_ONLY layout.
     * @param block The `VineShadowBlock`'s buffer range.
     */
    void writeShadowPair(VkDescriptorSet set, const VkDescriptorImageInfo& map, const VkDescriptorBufferInfo& block) const
    {
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = set;
        writes[0].dstBinding      = detail::kShadowMapBinding;
        writes[0].descriptorCount = 1U;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &map;
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = set;
        writes[1].dstBinding      = detail::kShadowBlockBinding;
        writes[1].descriptorCount = 1U;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[1].pBufferInfo     = &block;
        table->vkUpdateDescriptorSets(device, 2U, writes, 0U, nullptr);
    }

    /**
     * @brief Builds a content set: material at 0, the five slots at 1-5, lights at 6, the shadow pair at 7/8.
     *
     * @details
     * - EVERY UNIFORM BINDING IS `..._DYNAMIC` AND NAMES THE RING AT OFFSET 0: the draw's slice arrives as a dynamic
     *   offset at bind time (see record), which is what lets ONE set serve every draw that names the same resources.
     * - THE SHADOW ABI (see kShadowMapBinding): the engine's forward programs declare both, so every content set binds
     *   them - the map the pass samples, or the white stand-in when it samples none (see buildShadowBlock).
     *
     * @param slots          One image info per texture slot, in `TextureSlot` order.
     * @param shadow_view    The map this pass resolved, or the white stand-in.
     * @param shadow_sampler The map's sampler (the shadow pair's, not the material slots').
     * @return The set, or `VK_NULL_HANDLE`.
     */
    [[nodiscard]] VkDescriptorSet buildContentSet(const std::array<VkDescriptorImageInfo, detail::kTextureSlots>& slots,
                                                  const VkImageView                                        shadow_view,
                                                  const VkSampler                                          shadow_sampler) const
    {
        const VkDescriptorSet set = allocateSet(content_set_layout);
        if (set == VK_NULL_HANDLE) {
            return set;
        }

        VkDescriptorBufferInfo material_info{};
        material_info.buffer = ring.buffer;
        material_info.offset = 0U;
        material_info.range  = sizeof(vn::graphics::VineMaterialBlock);

        VkDescriptorBufferInfo lights_info{};
        lights_info.buffer = ring.buffer;
        lights_info.offset = 0U;
        lights_info.range  = sizeof(vn::graphics::VineLightsBlock);

        VkDescriptorImageInfo shadow_map{};
        shadow_map.sampler     = shadow_sampler;
        shadow_map.imageView   = shadow_view;
        // THE SHADOW PAIR STATES THE TABLE'S SAMPLED ROW (see BarrierPlan): the pass moved the picture there through the
        // same row, so the descriptor and the barrier cannot disagree.
        shadow_map.imageLayout = stateOf(ImageLayout::ShaderReadOnly).layout;

        VkDescriptorBufferInfo shadow_info{};
        shadow_info.buffer = ring.buffer;
        shadow_info.offset = 0U;
        shadow_info.range  = sizeof(vn::graphics::VineShadowBlock);

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = set;
        writes[0].dstBinding      = detail::kMaterialBinding;
        writes[0].descriptorCount = 1U;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[0].pBufferInfo     = &material_info;
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = set;
        writes[1].dstBinding      = detail::kTextureBindingBase;
        writes[1].descriptorCount = detail::kTextureSlots;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = slots.data();
        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = set;
        writes[2].dstBinding      = detail::kLightsBinding;
        writes[2].descriptorCount = 1U;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[2].pBufferInfo     = &lights_info;
        table->vkUpdateDescriptorSets(device, 3U, writes, 0U, nullptr);
        writeShadowPair(set, shadow_map, shadow_info);
        return set;
    }

    /**
     * @brief Builds the per-draw set: the ring's draw block at binding 0, as a DYNAMIC uniform.
     *
     * @return The set, or `VK_NULL_HANDLE`.
     */
    [[nodiscard]] VkDescriptorSet buildDrawSet() const
    {
        const VkDescriptorSet set = allocateSet(draw_set_layout);
        if (set == VK_NULL_HANDLE) {
            return set;
        }

        VkDescriptorBufferInfo draw_info{};
        draw_info.buffer = ring.buffer;
        draw_info.offset = 0U;
        draw_info.range  = sizeof(vn::graphics::VineDrawBlock);

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = detail::kDrawBinding;
        write.descriptorCount = 1U;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        write.pBufferInfo     = &draw_info;
        table->vkUpdateDescriptorSets(device, 1U, &write, 0U, nullptr);
        return set;
    }

    /**
     * @brief Builds a full-screen set: the source's attachments at 0..4, the lights at 6, the shadow pair at 7/8.
     *
     * @details
     * - Same two rules as the content set: every uniform binding is DYNAMIC and names the ring at offset 0, and the
     *   set is cached by the IMAGES it names - so a pass that samples the same source and map reuses one set across
     *   draws and frames.
     *
     * @param slots          One image info per source binding (absent ones carry the fallback), in binding order.
     * @param shadow_view    The map this pass resolved, or the white stand-in.
     * @param shadow_sampler The map's sampler.
     * @return The set, or `VK_NULL_HANDLE`.
     */
    [[nodiscard]] VkDescriptorSet buildFullscreenSet(const std::array<VkDescriptorImageInfo, detail::kTextureSlots>& slots,
                                                     const VkImageView                                               shadow_view,
                                                     const VkSampler                                                 shadow_sampler) const
    {
        const VkDescriptorSet set = allocateSet(fullscreen_set_layout);
        if (set == VK_NULL_HANDLE) {
            return set;
        }

        VkDescriptorBufferInfo lights_info{};
        lights_info.buffer = ring.buffer;
        lights_info.offset = 0U;
        lights_info.range  = sizeof(vn::graphics::VineLightsBlock);

        VkDescriptorImageInfo shadow_map{};
        shadow_map.sampler     = shadow_sampler;
        shadow_map.imageView   = shadow_view;
        // THE SAME SAMPLED ROW as the content set (see buildContentSet and BarrierPlan).
        shadow_map.imageLayout = stateOf(ImageLayout::ShaderReadOnly).layout;

        VkDescriptorBufferInfo shadow_info{};
        shadow_info.buffer = ring.buffer;
        shadow_info.offset = 0U;
        shadow_info.range  = sizeof(vn::graphics::VineShadowBlock);

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = set;
        writes[0].dstBinding      = detail::kFullscreenSamplerBindingBase;
        writes[0].descriptorCount = detail::kTextureSlots;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = slots.data();
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = set;
        writes[1].dstBinding      = detail::kLightsBinding;
        writes[1].descriptorCount = 1U;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[1].pBufferInfo     = &lights_info;
        table->vkUpdateDescriptorSets(device, 2U, writes, 0U, nullptr);
        writeShadowPair(set, shadow_map, shadow_info);
        return set;
    }

    /** @brief Bytes one draw's slice occupies: the four blocks, padded so the next slice stays aligned. */
    [[nodiscard]] VkDeviceSize sliceSize() const noexcept
    {
        return detail::alignedUp(uniformSize(), uniform_alignment);
    }

    /**
     * @brief Makes the ring hold @p needed bytes, REPLACING it when it cannot - between frames only.
     *
     * @details
     * - REPLACING DESTROYS A BUFFER, so it may not happen while a command buffer that names it is being recorded: the
     *   two callers are create() (before any frame) and beginFrame() (after the previous frame was answered and
     *   before this one is recorded).
     * - EVERY CACHED SET NAMES THE OLD BUFFER, so the cache is dropped first.
     *
     * @param needed Bytes the ring must hold.
     * @return true when the ring can serve @p needed.
     */
    [[nodiscard]] bool ensureRing(const VkDeviceSize needed)
    {
        if (ring.buffer != VK_NULL_HANDLE && needed <= ring.capacity) {
            return true;
        }
        if (table == nullptr || allocator == nullptr) {
            return false;
        }
        // GROW BY DOUBLING: a scene whose constants creep up then grows once per doubling, not once per frame.
        const VkDeviceSize capacity = detail::alignedUp(std::max({ needed, detail::kInitialRingBytes, ring.capacity * 2U }), uniform_alignment);
        VkBuffer           buffer   = VK_NULL_HANDLE;
        MemorySlice        memory{};
        if (!createHostBuffer(capacity, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, buffer, memory)) {
            return false;
        }
        // THE POOL OWNS THE MAPPING (see MemoryAllocator::map): coherent memory needs no flush, so the ring writes
        // straight into the block and keeps the address.
        void* const mapped = allocator->map(memory);
        if (mapped == nullptr) {
            destroyBuffer(buffer, memory);
            return false;
        }
        clearSets();
        destroyRing();
        ring = UniformRing{ buffer, memory, static_cast<std::byte*>(mapped), capacity, 0U, 0U };
        ++ring_builds_;
        return true;
    }

    /**
     * @brief Hands out the next slice of the frame's ring.
     *
     * @details
     * - A FULL RING DOES NOT GROW IN PLACE (see ensureRing): the demand is remembered and this call answers false, so
     *   the caller skips that draw. beginFrame grows to the demand before the next frame records anything.
     *
     * @param slice Receives the slice's byte offset.
     * @return true when the slice was handed out.
     */
    [[nodiscard]] bool reserveSlice(VkDeviceSize& slice) noexcept
    {
        const VkDeviceSize size = sliceSize();
        if (ring.buffer == VK_NULL_HANDLE || ring.cursor + size > ring.capacity) {
            ring.demand = std::max(ring.demand, ring.cursor + size);
            return false;
        }
        slice = ring.cursor;
        ring.cursor += size;
        return true;
    }

    /** @brief Frees every cached set (the ring they name is going away, or the pool is being rebuilt). */
    void clearSets() noexcept
    {
        for (auto& [key, entry] : sets) {
            (void)key;
            freeSet(entry.set);
        }
        sets.clear();
        freeSet(draw_set);
    }

    /** @brief Frees sets nobody has used for kSetRetentionFrames (see beginFrame). */
    void sweepSets() noexcept
    {
        for (auto entry = sets.begin(); entry != sets.end();) {
            if (entry->second.last_frame + detail::kSetRetentionFrames < frame_index) {
                freeSet(entry->second.set);
                entry = sets.erase(entry);
            }
            else {
                ++entry;
            }
        }
    }

    /** @brief Frees one set if it exists (a no-op before a pool does). */
    void freeSet(VkDescriptorSet& set) noexcept
    {
        if (set != VK_NULL_HANDLE && descriptor_pool != VK_NULL_HANDLE && table != nullptr) {
            (void)table->vkFreeDescriptorSets(device, descriptor_pool, 1U, &set);
        }
        set = VK_NULL_HANDLE;
    }

    /** @brief Destroys the ring's buffer and gives its slice back to the pool (idempotent). */
    void destroyRing() noexcept
    {
        // The mapping belongs to the pool's block (see MemoryAllocator::map): it is not unmapped here.
        destroyBuffer(ring.buffer, ring.memory);
        ring = UniformRing{};
    }

    /** @brief The key a set is cached under (see SetKey). */
    [[nodiscard]] static SetKey keyOf(const bool                                                      fullscreen,
                                      const std::array<VkDescriptorImageInfo, detail::kTextureSlots>& slots,
                                      const VkImageView                                               shadow,
                                      const VkSampler                                                 shadow_sampler) noexcept
    {
        SetKey key;
        key.fullscreen = fullscreen;
        for (std::size_t slot = 0; slot < detail::kTextureSlots; ++slot) {
            key.views[slot]    = slots[slot].imageView;
            key.samplers[slot] = slots[slot].sampler;
        }
        key.shadow         = shadow;
        key.shadow_sampler = shadow_sampler;
        return key;
    }

    /** @brief Answers the set cached under @p key, building it once (see SetKey). */
    template <typename Build>
    [[nodiscard]] VkDescriptorSet cachedSet(const SetKey& key, Build&& build)
    {
        const auto found = sets.find(key);
        if (found != sets.end()) {
            found->second.last_frame = frame_index;
            return found->second.set;
        }
        const VkDescriptorSet set = build();
        if (set == VK_NULL_HANDLE) {
            return set;
        }
        sets.emplace(key, SetEntry{ set, frame_index });
        ++set_builds_;
        return set;
    }

    /** @brief The content set for one drawable's resources (see buildContentSet). */
    [[nodiscard]] VkDescriptorSet contentSetFor(const std::array<VkDescriptorImageInfo, detail::kTextureSlots>& slots,
                                                const VkImageView                                        shadow_view,
                                                const VkSampler                                          shadow_sampler)
    {
        return cachedSet(keyOf(false, slots, shadow_view, shadow_sampler), [&] { return buildContentSet(slots, shadow_view, shadow_sampler); });
    }

    /** @brief The full-screen set for one call's source and map (see buildScreenSet). */
    [[nodiscard]] VkDescriptorSet
    fullscreenSetFor(const std::array<VkDescriptorImageInfo, detail::kTextureSlots>& slots, const VkImageView shadow_view, const VkSampler shadow_sampler)
    {
        return cachedSet(keyOf(true, slots, shadow_view, shadow_sampler), [&] { return buildFullscreenSet(slots, shadow_view, shadow_sampler); });
    }

    /** @brief The per-draw set: one handle per ring generation (its only binding is the ring's draw block). */
    [[nodiscard]] VkDescriptorSet drawSetFor()
    {
        if (draw_set == VK_NULL_HANDLE) {
            draw_set = buildDrawSet();
            if (draw_set != VK_NULL_HANDLE) {
                ++set_builds_;
            }
        }
        return draw_set;
    }
};

FrameScratch::FrameScratch()
  : impl(std::make_unique<Impl>())
{
}

FrameScratch::~FrameScratch()
{
    destroy();
}

bool FrameScratch::create(const VolkDeviceTable&      table,
                          const VkDevice              device,
                          MemoryAllocator&            allocator,
                          const std::uint32_t         uniform_alignment,
                          const VkDescriptorSetLayout content_set_layout,
                          const VkDescriptorSetLayout draw_set_layout,
                          const VkDescriptorSetLayout fullscreen_set_layout)
{
    destroy();
    if (device == VK_NULL_HANDLE || content_set_layout == VK_NULL_HANDLE || draw_set_layout == VK_NULL_HANDLE || fullscreen_set_layout == VK_NULL_HANDLE) {
        return false;
    }

    impl->table              = &table;
    impl->device             = device;
    impl->allocator          = &allocator;
    impl->uniform_alignment  = uniform_alignment != 0U ? uniform_alignment : detail::kDefaultUniformAlignment;
    impl->content_set_layout = content_set_layout;
    impl->draw_set_layout    = draw_set_layout;
    impl->fullscreen_set_layout = fullscreen_set_layout;

    // ONE POOL FOR THE SET CACHE (see Impl::sets): a set is a BINDING OF RESOURCES, so the pool holds one per
    // (texture set, shadow map) instead of one per draw - and it is freed PER ENTRY (FREE_DESCRIPTOR_SET) rather than
    // reset every frame, because a cached set has to outlive the frame that built it.
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    // material, lights and the shadow block per content set; the draw block per draw set; lights and the shadow block
    // per full-screen set - four is the worst case per cached set.
    pool_sizes[0].descriptorCount = detail::kMaxCachedSets * 4U;
    pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // five material slots + the shadow map per content set; five sampled inputs + the shadow map per full-screen set.
    pool_sizes[1].descriptorCount = detail::kMaxCachedSets * 2U * (detail::kTextureSlots + 1U);

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets       = detail::kMaxCachedSets;
    pool_info.poolSizeCount = 2U;
    pool_info.pPoolSizes    = pool_sizes;
    if (impl->table->vkCreateDescriptorPool(impl->device, &pool_info, nullptr, &impl->descriptor_pool) != VK_SUCCESS) {
        destroy();
        return false;
    }

    // THE UNIFORM RING IS PART OF COMING UP: every draw writes its constants into it, so a driver that cannot hold
    // 256 KiB of constants cannot draw anything (see UniformRing).
    if (!impl->ensureRing(detail::kInitialRingBytes)) {
        destroy(); // the device reports why a driver that cannot hold its constants did not come up
        return false;
    }
    return true;
}

void FrameScratch::destroy() noexcept
{
    if (!impl) {
        return;
    }
    if (impl->device != VK_NULL_HANDLE && impl->table != nullptr) {
        impl->clearSets();
        impl->destroyRing();
        if (impl->descriptor_pool != VK_NULL_HANDLE) {
            impl->table->vkDestroyDescriptorPool(impl->device, impl->descriptor_pool, nullptr);
            impl->descriptor_pool = VK_NULL_HANDLE;
        }
    }
    impl->table              = nullptr;
    impl->device             = VK_NULL_HANDLE;
    impl->allocator          = nullptr;
    impl->content_set_layout = VK_NULL_HANDLE;
    impl->draw_set_layout    = VK_NULL_HANDLE;
    impl->fullscreen_set_layout = VK_NULL_HANDLE;
    impl->ring_builds_       = 0;
    impl->set_builds_        = 0;
    impl->frame_index        = 0;
}

bool FrameScratch::ready() const noexcept
{
    return impl->device != VK_NULL_HANDLE && impl->table != nullptr;
}

void FrameScratch::beginFrame()
{
    if (!ready()) {
        return;
    }
    ++impl->frame_index;
    // A NEW FRAME'S SLICES START AT THE BEGINNING: the driver submits and waits inside executeFrame, so the previous
    // frame's reads are done (see UniformRing).
    impl->ring.cursor = 0;
    // THE FRAME THAT DID NOT FIT IS PAID FOR HERE, between frames: growing inside a frame would destroy a buffer the
    // command buffer being recorded already names (see ensureRing). A growth that fails leaves the demand standing, so
    // the next frame tries again and the draws that do not fit keep saying so.
    if (impl->ring.demand > impl->ring.capacity) {
        if (impl->ensureRing(impl->ring.demand)) {
            impl->ring.demand = 0;
        }
        // A GROWTH THAT FAILED KEEPS THE DEMAND: the next frame tries again, and the draws that do not fit keep
        // reporting through their own note (see reserveSlice).
    }
    if (impl->frame_index % detail::kSetSweepIntervalFrames == 0U) {
        impl->sweepSets();
    }
}

VkDeviceSize FrameScratch::lightsOffset() const noexcept
{
    return impl->lightsOffset();
}

VkDeviceSize FrameScratch::drawOffset() const noexcept
{
    return impl->drawOffset();
}

VkDeviceSize FrameScratch::shadowOffset() const noexcept
{
    return impl->shadowOffset();
}

VkDeviceSize FrameScratch::uniformSize() const noexcept
{
    return impl->uniformSize();
}

bool FrameScratch::reserveSlice(VkDeviceSize& slice) noexcept
{
    return impl->reserveSlice(slice);
}

std::byte* FrameScratch::slicePointer(const VkDeviceSize slice) const noexcept
{
    return impl->ring.mapped != nullptr ? impl->ring.mapped + slice : nullptr;
}

VkDescriptorSet FrameScratch::contentSetFor(const std::array<VkDescriptorImageInfo, detail::kTextureSlots>& slots,
                                            const VkImageView                                        shadow_view,
                                            const VkSampler                                          shadow_sampler)
{
    return impl->contentSetFor(slots, shadow_view, shadow_sampler);
}

VkDescriptorSet FrameScratch::fullscreenSetFor(const std::array<VkDescriptorImageInfo, detail::kTextureSlots>& slots,
                                               const VkImageView                                               shadow_view,
                                               const VkSampler                                                 shadow_sampler)
{
    return impl->fullscreenSetFor(slots, shadow_view, shadow_sampler);
}

VkDescriptorSet FrameScratch::drawSetFor()
{
    return impl->drawSetFor();
}

std::uint64_t FrameScratch::ringBuilds() const noexcept
{
    return impl->ring_builds_;
}

std::uint64_t FrameScratch::setBuilds() const noexcept
{
    return impl->set_builds_;
}

std::uint64_t FrameScratch::setsLive() const noexcept
{
    return static_cast<std::uint64_t>(impl->sets.size()) + (impl->draw_set != VK_NULL_HANDLE ? 1U : 0U);
}

VN_BACKEND_VK_NS_END

