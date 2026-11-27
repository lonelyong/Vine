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
    const std::lock_guard<std::mutex> lock(tables_mutex_);
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
    const std::lock_guard<std::mutex> lock(tables_mutex_);
    host_outputs_.erase(name);
    // Withdrawing the name ends the "cannot serve it" episode: publishing it again with no target is a new
    // mistake, not the same one.
    unpublishable_host_names_.erase(name);
}

const std::map<String, intrusive_ptr<RenderTarget>>& FrameResources::frameBindings() const noexcept
{
    // The compile's own copy: written by beginFrame, read only by the frame that took it (see the class note).
    return frame_bindings_;
}

std::vector<String> FrameResources::hostBindingNames() const
{
    const std::lock_guard<std::mutex> lock(tables_mutex_);
    std::vector<String>               names;
    names.reserve(host_outputs_.size());
    for (const auto& binding : host_outputs_) {
        names.push_back(binding.first);
    }
    return names;
}

void FrameResources::beginFrame()
{
    // THE HEAD FREEZES THE HOST'S SIDE (C23): the bindings are copied once, so every later question of this frame
    // answers the same set even if the host publishes from another thread while the compile runs. Copying under the
    // lock is what makes the copy itself safe; what follows is the compile's own tables, which no host verb touches.
    {
        const std::lock_guard<std::mutex> lock(tables_mutex_);
        frame_bindings_ = host_outputs_;
        publications_.clear();
    }
    produced_.clear();
}

void FrameResources::seedFromHostBindings()
{
    for (const auto& binding : frame_bindings_) {
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

raw_ptr<RenderTarget> FrameResources::recordPublication(raw_ptr<const RenderPass> pass, raw_ptr<RenderTarget> target)
{
    if (pass == nullptr) {
        return nullptr;
    }
    const String                      name = pass->outputName(); // asked once: the key of this row
    const std::lock_guard<std::mutex> lock(tables_mutex_);
    for (FramePublication& publication : publications_) {
        if (publication.pass != nullptr && publication.pass->outputName() == name) {
            raw_ptr<RenderTarget> previous = publication.target; // what the collision report compares against
            publication.target             = target;             // the latest publisher serves the name from here on
            return previous;
        }
    }
    publications_.push_back(FramePublication{ pass, target });
    return nullptr;
}

raw_ptr<RenderTarget> FrameResources::resolve(const String& name) const noexcept
{
    // THE HOST'S ANSWER: this frame's publications (locked, because the compile is writing them) and the LIVE
    // standing binding - a host that just published asks what it just published.
    const std::lock_guard<std::mutex> lock(tables_mutex_);
    for (const FramePublication& publication : publications_) {
        if (publication.pass != nullptr && publication.pass->outputName() == name) {
            return publication.target;
        }
    }
    const auto bound = host_outputs_.find(name);
    return (bound != host_outputs_.end()) ? bound->second.get() : nullptr;
}

raw_ptr<RenderTarget> FrameResources::resolveForFrame(const String& name) const noexcept
{
    // THE FRAME'S ANSWER: the same publications, but the bindings the head took - so a host publish landing
    // mid-compile answers on the next frame instead of answering one row of this one.
    const std::lock_guard<std::mutex> lock(tables_mutex_);
    for (const FramePublication& publication : publications_) {
        if (publication.pass != nullptr && publication.pass->outputName() == name) {
            return publication.target;
        }
    }
    const auto bound = frame_bindings_.find(name);
    return (bound != frame_bindings_.end()) ? bound->second.get() : nullptr;
}

void FrameResources::beginTransientCompile()
{
    const std::lock_guard<std::mutex> lock(tables_mutex_);
    for (TransientTarget& transient : transients_) {
        transient.declared_now = false;
    }
}

intrusive_ptr<RenderTarget> FrameResources::resolveTransient(const String& identity, const TransientShape& shape, int width, int height, RenderDevice* device)
{
    if (identity.empty()) {
        return nullptr;
    }
    intrusive_ptr<RenderTarget> replaced; // let go by a shape change: told to the device AFTER the lock
    intrusive_ptr<RenderTarget> answer;
    {
        const std::lock_guard<std::mutex> lock(tables_mutex_);
        TransientTarget*                  declared = nullptr;
        for (TransientTarget& transient : transients_) {
            if (transient.identity == identity) {
                declared = &transient;
                break;
            }
        }
        if (declared != nullptr) {
            if (!sameTransientShape(declared->shape, shape) || declared->target->width() != width || declared->target->height() != height) {
                // A different shape is a different TARGET: the attachments of a built one cannot change (see
                // RenderTarget), so the compile replaces it and tells the backend the old one is done with. The build
                // stays under the lock (it is an allocation, not a device call), so a host lookup never sees a row
                // whose target is momentarily missing.
                replaced         = std::move(declared->target);
                declared->shape  = shape;
                declared->target = makeTransientTarget(identity, width, height, shape);
            }
            declared->declared_now = true;
            answer                 = declared->target;
        }
        else {
            TransientTarget created;
            created.identity     = identity;
            created.shape        = shape;
            created.target       = makeTransientTarget(identity, width, height, shape);
            created.declared_now = true;
            answer               = created.target;
            transients_.push_back(std::move(created));
        }
    }
    // THE DEVICE IS NOT TOLD UNDER THE LOCK: releaseRenderTarget takes the backend's own locks, and holding this one
    // across them would put the table's lock above the device's in an order nothing else states.
    release(replaced.get(), device);
    return answer;
}

void FrameResources::retireUndeclared(RenderDevice* device)
{
    std::vector<intrusive_ptr<RenderTarget>> retired; // emptied normally; a shape change is what fills it
    {
        const std::lock_guard<std::mutex> lock(tables_mutex_);
        for (auto transient = transients_.begin(); transient != transients_.end();) {
            if (transient->declared_now) {
                ++transient;
                continue;
            }
            retired.push_back(std::move(transient->target));
            transient = transients_.erase(transient);
        }
    }
    for (const intrusive_ptr<RenderTarget>& target : retired) {
        release(target.get(), device);
    }
}

std::size_t FrameResources::transientCount() const noexcept
{
    const std::lock_guard<std::mutex> lock(tables_mutex_);
    return transients_.size();
}

raw_ptr<RenderTarget> FrameResources::transient(const String& identity) const noexcept
{
    if (identity.empty()) {
        return nullptr;
    }
    const std::lock_guard<std::mutex> lock(tables_mutex_);
    for (const TransientTarget& transient : transients_) {
        if (transient.identity == identity) {
            return transient.target.get();
        }
    }
    return nullptr;
}

bool FrameResources::owns(raw_ptr<const RenderTarget> target) const noexcept
{
    const std::lock_guard<std::mutex> lock(tables_mutex_);
    return std::any_of(transients_.begin(), transients_.end(), [target](const TransientTarget& transient) { return transient.target.get() == target; });
}

void FrameResources::releaseAll(RenderDevice* device)
{
    std::vector<intrusive_ptr<RenderTarget>> released;
    {
        const std::lock_guard<std::mutex> lock(tables_mutex_);
        released.reserve(transients_.size());
        for (TransientTarget& transient : transients_) {
            released.push_back(std::move(transient.target));
        }
        transients_.clear();
    }
    for (const intrusive_ptr<RenderTarget>& target : released) {
        release(target.get(), device);
    }
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
