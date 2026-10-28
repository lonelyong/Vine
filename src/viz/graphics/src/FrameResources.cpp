#include "FrameResources.hpp"

#include <algorithm>
#include <utility>

#include <vine/graphics/RenderDevice.hpp>
#include <vine/graphics/RenderPass.hpp>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

namespace
{

/// @brief Whether two declared transient shapes describe the same target.
///
/// A shape is what the target is BUILT from, so a difference here is a different target rather than an edit to
/// this one: the attachment set cannot change under a built target (see RenderTarget).
bool sameTransientShape(const TransientShape& left, const TransientShape& right) noexcept
{
    return left.colors == right.colors && left.depth == right.depth && left.depth_promotion == right.depth_promotion;
}

} // namespace

FrameResources::FrameResources() = default;

FrameResources::~FrameResources() = default;

String FrameResources::publish(const String& name, intrusive_ptr<RenderTarget> target)
{
    if (name.empty()) {
        return {}; // no name: nothing to serve, and nothing a consumer could have asked for
    }
    if (target == nullptr) {
        // A null target means there is nothing to hand out. The episode is the NAME: it ends when a publish()
        // hands over a real target (or unpublish() withdraws it), because a host has no frame to re-publish from.
        if (unpublishable_host_names_.insert(name).second) {
            return String(u8"publish('") + name +
                   String(u8"') was given no render target, so the name serves nothing and its"
                          u8" consumers find nothing — pass the target to hand out, or unpublish()"
                          u8" the name");
        }
        return {};
    }
    unpublishable_host_names_.erase(name);
    // A HOST binding, not a pass publication: it keeps until unpublish() removes it (or another publish replaces
    // it), because a host has no per-frame hook to re-publish from.
    host_outputs_[name] = std::move(target);
    return {};
}

void FrameResources::unpublish(const String& name)
{
    // A pass' publication lives only inside the frame that compiled it, so the only binding to withdraw is the
    // host's: the next compile stops answering the name on its own.
    host_outputs_.erase(name);
    // Withdrawing the name ends the "cannot serve it" episode: publishing it again with no target is a new
    // mistake, not the same one.
    unpublishable_host_names_.erase(name);
}

const std::map<String, intrusive_ptr<RenderTarget>>& FrameResources::hostBindings() const noexcept
{
    return host_outputs_;
}

void FrameResources::beginFrame()
{
    produced_.clear();
    publications_.clear();
}

void FrameResources::seedFromHostBindings()
{
    for (const auto& binding : host_outputs_) {
        if (binding.second != nullptr) {
            recordProduced(binding.second.get());
        }
    }
}

bool FrameResources::wasProduced(raw_ptr<const RenderTarget> target) const noexcept
{
    return std::find(produced_.begin(), produced_.end(), target) != produced_.end();
}

void FrameResources::recordProduced(raw_ptr<const RenderTarget> target)
{
    if (target == nullptr) {
        return;
    }
    if (!wasProduced(target)) {
        produced_.push_back(target);
    }
}

FramePublication* FrameResources::findPublication(const String& name) noexcept
{
    for (FramePublication& publication : publications_) {
        if (publication.pass != nullptr && publication.pass->outputName() == name) {
            return &publication;
        }
    }
    return nullptr;
}

void FrameResources::recordPublication(raw_ptr<const RenderPass> pass, raw_ptr<RenderTarget> target)
{
    if (pass == nullptr) {
        return;
    }
    publications_.push_back(FramePublication{ pass, target });
}

raw_ptr<RenderTarget> FrameResources::resolve(const String& name) const noexcept
{
    for (const FramePublication& publication : publications_) {
        if (publication.pass != nullptr && publication.pass->outputName() == name) {
            return publication.target;
        }
    }
    const auto bound = host_outputs_.find(name);
    return (bound != host_outputs_.end()) ? bound->second.get() : nullptr;
}

void FrameResources::beginTransientCompile()
{
    for (TransientTarget& transient : transients_) {
        transient.declared_now = false;
    }
}

intrusive_ptr<RenderTarget> FrameResources::resolveTransient(const String& identity, const TransientShape& shape, int width, int height, RenderDevice* device)
{
    if (identity.empty()) {
        return nullptr;
    }
    for (TransientTarget& transient : transients_) {
        if (transient.identity != identity) {
            continue;
        }
        if (!sameTransientShape(transient.shape, shape) || transient.target->width() != width || transient.target->height() != height) {
            // A different shape is a different TARGET: the attachments of a built one cannot change (see
            // RenderTarget), so the compile replaces it and tells the backend the old one is done with.
            release(transient.target.get(), device);
            transient.shape  = shape;
            transient.target = makeTransientTarget(identity, width, height, shape);
        }
        transient.declared_now = true;
        return transient.target;
    }

    TransientTarget created;
    created.identity     = identity;
    created.shape        = shape;
    created.target       = makeTransientTarget(identity, width, height, shape);
    created.declared_now = true;
    transients_.push_back(created);
    return created.target;
}

void FrameResources::retireUndeclared(RenderDevice* device)
{
    for (auto transient = transients_.begin(); transient != transients_.end();) {
        if (transient->declared_now) {
            ++transient;
            continue;
        }
        release(transient->target.get(), device);
        transient = transients_.erase(transient);
    }
}

std::size_t FrameResources::transientCount() const noexcept
{
    return transients_.size();
}

raw_ptr<RenderTarget> FrameResources::transient(const String& identity) const noexcept
{
    if (identity.empty()) {
        return nullptr;
    }
    for (const TransientTarget& transient : transients_) {
        if (transient.identity == identity) {
            return transient.target.get();
        }
    }
    return nullptr;
}

bool FrameResources::owns(raw_ptr<const RenderTarget> target) const noexcept
{
    return std::any_of(transients_.begin(), transients_.end(), [target](const TransientTarget& transient) { return transient.target.get() == target; });
}

void FrameResources::releaseAll(RenderDevice* device)
{
    for (const TransientTarget& transient : transients_) {
        release(transient.target.get(), device);
    }
    transients_.clear();
}

intrusive_ptr<RenderTarget> FrameResources::makeTransientTarget(const String& identity, int width, int height, const TransientShape& shape)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(identity);
    for (const RenderTarget::ColorFormat format : shape.colors) {
        target->attachColor(format);
    }
    if (shape.depth.has_value()) {
        target->attachDepth(*shape.depth);
    }
    target->setDepthPromotion(shape.depth_promotion);
    target->setSize(width, height);
    return target;
}

void FrameResources::release(raw_ptr<RenderTarget> target, RenderDevice* device)
{
    if (target == nullptr || device == nullptr) {
        return;
    }
    // The SDK's announcement that the caller may destroy it now: the backend drops what it built for it (and
    // retires anything still in flight), exactly as removePass() does for a pass' own target.
    device->releaseRenderTarget(target);
}

} // namespace detail

VN_GRAPHICS_NS_END
