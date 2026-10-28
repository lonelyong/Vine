#include <vine/vsg/internal/ContentSets.hpp>

#include <algorithm>
#include <optional>
#include <utility>

#include <vsg/state/Sampler.h>

#include <vine/graphics/Texture.hpp>
#include <vine/vsg/internal/ContentPipeline.hpp>
#include <vine/vsg/internal/ProgramVariant.hpp>

VN_VSG_NS_BEGIN

namespace
{

/** @brief Whether one declared binding is a sampled image (the kinds that need an image here). */
bool isSampled(const AbiBinding& binding) noexcept
{
    return binding.kind != AbiDescriptorKind::UniformBlock;
}

/** @brief Whether a declared sampler takes a CUBE (its fallback is the cube's, see api/MaterialImages). */
bool isCube(const AbiBinding& binding) noexcept
{
    return binding.type_name == "samplerCube";
}

/** @brief One image from the pass' inputs, walked in the order the pass' own input set binds them. */
struct InputWalk {
    std::span<const InputImages> inputs;         // The offered images.
    std::size_t                  input{ 0 };     // The input being read.
    std::size_t                  color{ 0 };     // The next colour attachment of it.
    bool                         depth{ false }; // Whether that input's depth was already taken.

    /** @brief Gets the next image the pass' input set would bind, or null when the inputs ran out.
     *
     * Each input carries its own declared state when it has one (an imported input); an input that stated
     * nothing uses @p default_sampler, and its depth uses @p default_depth_sampler. The walk is the same one
     * the input set itself binds, so a by-name sampler and a declaration-order sampler see one texture.
     *
     * @param default_sampler       Sampler for a binding that declared none.
     * @param default_depth_sampler Sampler for a depth binding that declared none.
     * @return The next image and its sampler, or an empty value when the inputs ran out.
     */
    [[nodiscard]] SamplerImage next(const ::vsg::ref_ptr<::vsg::Sampler>& default_sampler, const ::vsg::ref_ptr<::vsg::Sampler>& default_depth_sampler) noexcept
    {
        while (input < inputs.size()) {
            const InputImages& entry = inputs[input];
            if (color < entry.colors.size()) {
                const ::vsg::ref_ptr<::vsg::ImageView>& view   = entry.colors[color];
                const ::vsg::ref_ptr<::vsg::Sampler>    chosen = color < entry.color_samplers.size() ? entry.color_samplers[color] : default_sampler;
                ++color;
                if (view != nullptr) {
                    return SamplerImage{ view, chosen };
                }
                continue;
            }
            if (entry.depth != nullptr && !depth) {
                depth = true;
                return SamplerImage{ entry.depth, entry.depth_sampler != nullptr ? entry.depth_sampler : default_depth_sampler };
            }
            ++input;
            color = 0U;
            depth = false;
        }
        return SamplerImage{};
    }
};

/** @brief Whether the set declares a sampler whose image is the drawable's own (see the file note). */
bool declaresMaterialSampler(const ContentPipeline& layer, std::uint32_t set) noexcept
{
    for (const AbiBinding& binding : layer.abi().bindings) {
        if (binding.set == set && isSampled(binding) && imageOriginOf(binding.name) == ImageOrigin::Material) {
            return true;
        }
    }
    return false;
}

/** @brief Whether the set declares the shadow map - an image the PASS owns, not the drawable's. */
bool declaresShadowSampler(const ContentPipeline& layer, std::uint32_t set) noexcept
{
    for (const AbiBinding& binding : layer.abi().bindings) {
        if (binding.set == set && isSampled(binding) && imageOriginOf(binding.name) == ImageOrigin::Shadow) {
            return true;
        }
    }
    return false;
}

} // namespace

struct ContentSets::Data {
    /** @brief What one set answers for (see the file note). */
    struct Key {
        const void*                   program{ nullptr };
        std::uint64_t                 revision{ 0 };
        ProgramVariant                variant{};
        std::uint32_t                 set{ 0 };
        BlockDescriptors::ImageSource source{};
        /// The shadow map the set binds, or null when it declares none.
        ///
        /// PART OF THE KEY because the map arrives LATE: a recording that builds this set before the shadow
        /// pass has promoted its depth (a warm-up frame, or the first frame of a session) resolves nothing and
        /// the set is built with the stand-in white image - whose `texture().r` is 1.0, i.e. "a caster at the
        /// near plane for every fragment". Keyed by the image, that set stops being a hit the moment the real
        /// map exists; unkeyed, it answered for the rest of the session and the whole picture shaded as shadowed
        /// (measured 2026-10-24 on the forward path, where the map reaches the draws through a CONTENT set - the
        /// full-screen path builds its set per call and was unaffected).
        const void* shadow{ nullptr };

        [[nodiscard]] friend bool operator==(const Key&, const Key&) noexcept = default;
    };

    struct Set {
        Key                               key{};
        std::unique_ptr<BlockDescriptors> descriptors{}; // Null when the set was REFUSED (not retried).
    };

    ::vsg::ref_ptr<::vsg::Device> device{};
    const BlockStorage*           storage{ nullptr };
    MaterialImages*               images{ nullptr };

    std::vector<std::shared_ptr<Set>> sets;
    std::vector<BlockDescriptors*>    candidates;
    /// Sets a repoint replaced where the parking window was not open: kept until the producer ends
    /// (never freed early - see repoint).
    std::vector<::vsg::ref_ptr<::vsg::DescriptorSet>> kept_replaced;
    std::uint64_t                                     builds{ 0 };
    std::size_t                                       fallbacks{ 0 };
    std::size_t                                       refused{ 0 };

    /** @brief Gets the set one key already has (a refused one is a hit too: it must not be retried). */
    [[nodiscard]] Set* find(const Key& key) noexcept
    {
        for (const std::shared_ptr<Set>& set : sets) {
            if (set->key == key) {
                return set.get();
            }
        }
        return nullptr;
    }

    /** @brief Adds @p set to this call's candidate list, once. */
    void offer(Set& set)
    {
        if (set.descriptors == nullptr) {
            return;
        }
        for (const BlockDescriptors* candidate : candidates) {
            if (candidate == set.descriptors.get()) {
                return;
            }
        }
        candidates.push_back(set.descriptors.get());
    }

    /** @brief Ensures the set one key names, resolving its images from @p key's source and the pass' inputs. */
    void ensure(const Key& key, const ContentPass::Scope::Entry& entry, const core::CompiledPass& pass, std::span<const InputImages> inputs)
    {
        if (Set* existing = find(key)) {
            offer(*existing);
            return;
        }

        const ProgramAbi&                             abi = entry.pipelines->abi();
        std::vector<BlockDescriptors::SampledBinding> sampled;
        InputWalk                                     walk{ inputs, 0U, 0U, false };
        for (const AbiBinding& binding : abi.bindings) {
            if (binding.set != key.set || !isSampled(binding)) {
                continue;
            }
            SamplerImage map;
            switch (imageOriginOf(binding.name)) {
            case ImageOrigin::Material:
            {
                detail::TextureReject reason                    = detail::TextureReject::Ok;
                // WHICH SLOT: the name is the contract (see api/ContentImages::materialSlotOf). A material
                // binding that names no slot has no image to offer - the declared kind's white stands in, the
                // same answer as an absent slot.
                const std::optional<std::size_t>           slot = materialSlotOf(binding.name);
                const BlockDescriptors::ImageSource::Slot  absent{ nullptr, 0U, vn::graphics::Sampler{} };
                const BlockDescriptors::ImageSource::Slot& source = slot.has_value() && *slot < key.source.slots.size() ? key.source.slots[*slot] : absent;
                map                      = images->acquire(static_cast<const vn::graphics::Texture*>(source.texture), source.sampler, reason);
                // The DECLARED kind is the contract a descriptor write has to satisfy: a cube declaration
                // cannot take a 2D view and a 2D declaration cannot take a view of a six-layer image (an
                // invalid descriptor, not a wrong picture - see MaterialImages' fallback note). A map that is
                // absent, unusable or of the OTHER kind is therefore answered with the DECLARED kind's white
                // - "no usable map" is a VALUE here, the same way it is for an absent texture - and counted,
                // so a host can see that its material did not fit the text it was drawn with.
                const bool cube_declared = isCube(binding);
                const bool cube_image    = map.view != nullptr && map.view->viewType == VK_IMAGE_VIEW_TYPE_CUBE;
                if (source.texture == nullptr || reason != detail::TextureReject::Ok || map.view == nullptr || cube_declared != cube_image) {
                    ++fallbacks;
                    map = cube_declared ? images->whiteCube() : images->white();
                }
                break;
            }
            case ImageOrigin::Shadow:
            {
                if (!shadowImageOf(pass, inputs, entry.pipelines->depthSampler(), map)) {
                    map = isCube(binding) ? images->whiteCube() : images->white();
                    ++fallbacks;
                }
                break;
            }
            case ImageOrigin::Input:
            {
                // An Input-named sampler inside a DECLARED set is unusual but expressible - it takes the
                // pass' images in the order the pass' own input set binds them, each with the state its
                // declaration named (the layer's samplers when it named none).
                map = walk.next(entry.pipelines->inputSampler(), entry.pipelines->depthSampler());
                break;
            }
            }
            sampled.push_back(BlockDescriptors::SampledBinding{ binding.binding, map.view, map.sampler });
        }

        ++builds;
        std::shared_ptr<Set> set = std::make_shared<Set>();
        set->key                 = key;
        set->descriptors         = BlockDescriptors::forAbi(abi, key.set, device, *storage, sampled, key.source);
        if (set->descriptors == nullptr) {
            ++refused;
            sets.push_back(std::move(set));
            return;
        }
        sets.push_back(set);
        offer(*sets.back());
    }
};

ContentSets::ContentSets(::vsg::ref_ptr<::vsg::Device> device, const BlockStorage& storage, MaterialImages& images)
  : d(std::make_shared<Data>())
{
    d->device  = std::move(device);
    d->storage = &storage;
    d->images  = &images;
}

BlockDescriptors::ImageSource imageSourceOf(const MaterialFacts& material) noexcept
{
    BlockDescriptors::ImageSource source{};
    for (std::size_t index = 0; index < material.slots.size(); ++index) {
        const MaterialSlotFacts& slot = material.slots[index];
        if (slot.texture != nullptr) {
            source.slots[index].texture  = slot.texture;
            source.slots[index].revision = slot.texture->revision();
            source.slots[index].sampler  = slot.sampler;
        }
    }
    return source;
}

std::size_t ContentSets::repoint(const BlockStorage& storage, core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    d->storage            = &storage;
    std::size_t repointed = 0;
    for (const std::shared_ptr<Data::Set>& set : d->sets) {
        if (set == nullptr || set->descriptors == nullptr) {
            continue; // a REFUSED set: there is nothing to repoint, and a retry answers the same way
        }
        const ::vsg::ref_ptr<::vsg::DescriptorSet> previous = set->descriptors->set();
        if (!set->descriptors->repoint(storage)) {
            continue;
        }
        ++repointed;
        // The set that was replaced keeps its VkDescriptorSet handle, and the pool may not hand that handle
        // back while anything still names it - so it leaves through the window every replaced object gets
        // (and when there is no window to park through yet, it is KEPT: memory spent, never freed early).
        if (previous != nullptr && !retirement.retire(timeline, [previous]() {})) {
            d->kept_replaced.push_back(previous);
        }
    }
    return repointed;
}

ContentSets::~ContentSets() = default;

std::span<BlockDescriptors* const> ContentSets::setsFor(const core::CompiledPass&                  pass,
                                                        const ContentFacts&                        facts,
                                                        std::span<const ContentPass::Scope::Entry> halves,
                                                        std::span<const InputImages>               inputs,
                                                        core::FrameTimeline&                       timeline,
                                                        core::RetirementQueue&                     retirement)
{
    d->candidates.clear();

    for (const core::CompiledDraw& draw : pass.draws) {
        if (draw.kind == core::DrawKind::Screen) {
            continue; // a full-screen call's set is the pass' own (declaredSets is empty for that ABI)
        }
        for (const core::CompiledCommand& command : draw.commands) {
            const FactResult<GeometryFacts> geometry = findGeometry(facts, command.geometry, command.geometry_revision);
            if (!geometry.found()) {
                continue;
            }
            const FactResult<MaterialFacts> material = findMaterial(facts, command.material);
            if (!material.found()) {
                continue;
            }
            ProgramVariant variant = variantOf(*material.entry, *geometry.entry);
            variant.instanced      = command.instance_buffer != nullptr;

            const ContentPass::Scope::Entry* entry = nullptr;
            for (const ContentPass::Scope::Entry& candidate : halves) {
                // The same tuple the recorder matches entries by (see api/ContentPass::recordCommand), the
                // topology included: a half is compiled for one class of primitives, and the sets are built
                // against the half the draw will actually go through.
                if (candidate.kind == core::DrawKind::Content && candidate.program == command.program.program &&
                    candidate.revision == command.program.revision && candidate.layout == geometry.entry->layout && candidate.variant == variant &&
                    candidate.topology == command.dynamic.topology)
                {
                    entry = &candidate;
                    break;
                }
            }
            if (entry == nullptr || entry->pipelines == nullptr) {
                continue; // no half serves it: the recorder refuses it on the half, and says so
            }

            // The images this drawable asks for: its material's five texture slots at the revisions those
            // textures are at (the triple api/MaterialImages keys by, and the tag the pass picks a set by).
            // VALUE-initialised: a material with no map states NO source, and an indeterminate slot here would
            // file such a drawable under the previous one's images (ImageSource has no default member
            // initialisers - see its own note - so the `{}` is the initialisation).
            const BlockDescriptors::ImageSource source = imageSourceOf(*material.entry);

            for (const std::uint32_t set : entry->pipelines->declaredSets()) {
                Data::Key key;
                key.program  = command.program.program;
                key.revision = command.program.revision;
                key.variant  = variant;
                key.set      = set;
                // A set whose samplers are all pass-level does not depend on the drawable's images: tagging
                // it with an empty source lets every drawable of this variant share it.
                key.source   = declaresMaterialSampler(*entry->pipelines, set) ? source : BlockDescriptors::ImageSource{};
                // The map is the PASS' image, resolved the way ensure() will resolve it - so the key states
                // what the set actually binds (see Key::shadow).
                if (declaresShadowSampler(*entry->pipelines, set)) {
                    vn::vsg::SamplerImage shadow_map;
                    if (vn::vsg::shadowImageOf(pass, inputs, entry->pipelines->depthSampler(), shadow_map)) {
                        key.shadow = shadow_map.view.get();
                    }
                }
                d->ensure(key, *entry, pass, inputs);
            }
        }
    }

    // The sweep, exactly as api/ContentHalves does it: a set whose (program, revision, variant) the tables no
    // longer answer can never be asked for again - a pass builds its keys from those same tables - so it is
    // parked through the same window every other replaced object gets.
    for (auto it = d->sets.begin(); it != d->sets.end();) {
        const std::shared_ptr<Data::Set> set = *it;
        const bool                       answerable =
            // The set's source program is asked for AS CONTENT: every set here belongs to a content half (a
            // full-screen call's set is the pass' own and never reaches this table - see this function's own
            // guard above), so the entry that keeps it alive is the content one (see ProgramFacts::kind).
            findProgram(facts, core::ProgramRef{ set->key.program, set->key.revision }, set->key.variant, core::DrawKind::Content).found();
        if (answerable) {
            ++it;
            continue;
        }
        if (set->descriptors != nullptr) {
            const bool parked = retirement.retire(timeline, [set]() { (void)set; });
            if (!parked) {
                ++it; // no window: kept, which costs memory and never correctness
                continue;
            }
        }
        it = d->sets.erase(it);
    }

    return d->candidates;
}

std::size_t ContentSets::sets() const noexcept
{
    std::size_t alive = 0U;
    for (const std::shared_ptr<Data::Set>& set : d->sets) {
        if (set->descriptors != nullptr) {
            ++alive;
        }
    }
    return alive;
}

std::uint64_t ContentSets::builds() const noexcept
{
    return d->builds;
}

std::size_t ContentSets::fallbacks() const noexcept
{
    return d->fallbacks;
}

std::size_t ContentSets::refused() const noexcept
{
    return d->refused;
}

void ContentSets::clear()
{
    d->sets.clear();
    d->candidates.clear();
    d->fallbacks = 0U;
    d->refused   = 0U;
}

VN_VSG_NS_END
