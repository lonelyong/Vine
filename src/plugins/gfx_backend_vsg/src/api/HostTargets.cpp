#include <vine/vsg/api/HostTargets.hpp>

#include <optional>

V_VSG_NS_BEGIN

void HostTargets::describe(const vine::graphics::RenderTarget& target, Description& out)
{
    out.width  = target.width();
    out.height = target.height();

    const std::int32_t color_count = target.colorCount();
    out.color_formats.resize(static_cast<std::size_t>(color_count > 0 ? color_count : 0));
    for (std::size_t index = 0; index < out.color_formats.size(); ++index)
    {
        out.color_formats[index] = target.colorFormat(static_cast<int>(index));
    }

    out.has_depth       = target.hasDepth();
    out.depth_format    = target.depthFormat();
    out.depth_promotion = target.depthPromotion();

    const vine::graphics::RenderTarget* source = target.depthSource();
    out.depth_source = source;

    out.shadow_light = target.shadowOf();
    out.has_view_projection = target.hasProducerViewProjection();
    if (out.has_view_projection)
    {
        out.view_projection = target.producerViewProjection();
    }
}

HostTargets::State HostTargets::build(Entry& entry, ::vsg::ref_ptr<::vsg::Device> device)
{
    const Description& description = entry.description;

    // A description that cannot make a target yet: no image of no size, or NOTHING to attach. A target needs
    // at least one image, and a DEPTH-ONLY one is a target like any other - the engine's shadow map is exactly
    // that (attachDepth with no colour attachment, and the shading samples its depth). The entry keeps the
    // description and the next call retries - a host configures a target before it draws into it.
    if (description.width <= 0 || description.height <= 0 ||
        (description.color_formats.empty() && !description.has_depth))
    {
        return State::NotBuilt;
    }

    OffscreenTarget::TargetLayout layout;
    layout.width         = static_cast<std::uint32_t>(description.width);
    layout.height        = static_cast<std::uint32_t>(description.height);
    layout.color_formats = description.color_formats;

    const OffscreenTarget* depth_source = nullptr;
    if (description.has_depth)
    {
        if (description.depth_source != nullptr)
        {
            // A borrowed depth: the lender's objects must be held (and built) here - the SDK's own shareDepth
            // rule is that the source renders earlier in the frame, which is why this call is the borrower's
            // and not a search for it. The depth's policy is the lender's, so this target never promises it to
            // a shader.
            if (entry.depth_owner == nullptr)
            {
                return State::DepthSourceMissing;
            }
            depth_source        = entry.depth_owner.get();
            layout.depth_format = entry.depth_owner->layout().depth_format;
        }
        else
        {
            layout.depth_format    = description.depth_format;
            layout.depth_sampleable = description.depth_promotion;
        }
    }

    entry.target = OffscreenTarget::create(std::move(device), layout, depth_source);
    if (entry.target == nullptr)
    {
        return State::BuildFailed;
    }
    return State::Ready;
}

HostTargets::Ensured HostTargets::ensure(const vine::graphics::RenderTarget& target, ::vsg::ref_ptr<::vsg::Device> device)
{
    Entry* entry = find(&target);
    if (entry == nullptr)
    {
        entries_.push_back(std::make_unique<Entry>());
        entry           = entries_.back().get();
        entry->identity = &target;
    }

    describe(target, entry->description);

    if (entry->target != nullptr)
    {
        return Ensured{ entry, State::Ready };  // built for a description the plan owns from here on
    }
    if (device == nullptr)
    {
        return Ensured{ entry, State::NotBuilt };  // nothing is up to build on; the description is kept
    }

    // What this target borrows its depth from: the entry of the announcing host object. Resolved HERE, at
    // the moment the host names it, because the SDK's contract makes the lender a call-order fact (the source
    // renders earlier in the frame) rather than something to look for later.
    if (entry->description.depth_source != nullptr && entry->depth_owner == nullptr)
    {
        const Entry* lender = find(entry->description.depth_source);
        if (lender != nullptr && lender->target != nullptr)
        {
            entry->depth_owner = lender->target;
        }
    }

    return Ensured{ entry, build(*entry, std::move(device)) };
}

HostTargets::Entry* HostTargets::observe(const vine::graphics::RenderTarget& target)
{
    Entry* entry = find(&target);
    if (entry == nullptr)
    {
        entries_.push_back(std::make_unique<Entry>());
        entry           = entries_.back().get();
        entry->identity = &target;
    }
    describe(target, entry->description);
    return entry;
}

HostTargets::Entry* HostTargets::find(const void* identity) noexcept
{
    if (identity == nullptr)
    {
        return nullptr;
    }
    for (const std::unique_ptr<Entry>& entry : entries_)
    {
        if (entry->identity == identity)
        {
            return entry.get();
        }
    }
    return nullptr;
}

bool HostTargets::release(const void* identity) noexcept
{
    if (identity == nullptr)
    {
        return false;
    }
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
    {
        if ((*it)->identity != identity)
        {
            continue;
        }
        // The lender's objects may still be referenced by a borrower's entry (see the file note): dropping
        // this entry drops only this registry's share of them.
        entries_.erase(it);
        return true;
    }
    return false;
}

void HostTargets::clear() noexcept
{
    entries_.clear();
}

std::size_t HostTargets::live() const noexcept
{
    return entries_.size();
}

const std::vector<std::unique_ptr<HostTargets::Entry>>& HostTargets::entries() const noexcept
{
    return entries_;
}

void HostTargets::facts(const Entry& entry, core::TargetFacts& out) const
{
    out = core::TargetFacts{};
    out.target = entry.identity;

    out.wanted.width  = entry.description.width;
    out.wanted.height = entry.description.height;

    // The engine half is the host's word, always stated. The DEVICE half is only stated while the attachments
    // were built for exactly these engine formats: a shape that changed has no device formats yet, and
    // "unknown" must not read as "absent" (see CompiledShape) - a fabricated one would be a compatibility the
    // plan could not tell from the old one.
    out.wanted.shape.color_formats = entry.description.color_formats;
    out.wanted.shape.depth_format  = entry.description.has_depth
                                         ? std::optional{ entry.description.depth_format }
                                         : std::optional<vine::graphics::RenderTarget::DepthFormat>{};

    if (entry.target != nullptr)
    {
        out.current = entry.target->instance();
        const bool same_engine_shape = out.current.desc.shape.color_formats == out.wanted.shape.color_formats &&
                                       out.current.desc.shape.depth_format == out.wanted.shape.depth_format;
        if (same_engine_shape)
        {
            const core::TargetShape& built = entry.target->shape();
            out.wanted.shape.device_color_formats = built.device_color_formats;
            out.wanted.shape.device_depth_format  = built.device_depth_format;
            out.wanted.shape.samples              = built.samples;
            out.wanted.shape.subpass              = built.subpass;
        }
    }

    // The depth facts: whether there is one, whether it is the host's own promotion or a lender's, and who
    // the lender is. Once built, the TARGET's own policy answers for the promotion - it is what the plan's
    // depth decision has to agree with (a rebuild keeps the target's own promotion, see OffscreenTarget).
    out.depth.has_depth = entry.description.has_depth;
    out.depth.borrowed  = entry.description.depth_source != nullptr;
    out.depth.source    = entry.description.depth_source;
    out.depth.promotion = !out.depth.borrowed &&
                          (entry.target != nullptr ? entry.target->layout().depth_sampleable
                                                   : entry.description.depth_promotion);

    // The shadow statement, copied from the host's own words: a target is a map for as long as it says so,
    // and how to read it is the producer's matrix, never a consumer's guess (see ShadowFacts).
    if (entry.description.shadow_light != nullptr)
    {
        out.shadow.light                = entry.description.shadow_light;
        out.shadow.has_view_projection  = entry.description.has_view_projection;
        out.shadow.view_projection      = entry.description.view_projection;
    }
}

V_VSG_NS_END
