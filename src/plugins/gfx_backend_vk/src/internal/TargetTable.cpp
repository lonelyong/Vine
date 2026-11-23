#include <vine/backend_vk/internal/TargetTable.hpp>

#include <algorithm>
#include <optional>
#include <utility>

#include <vine/backend_vk/internal/NumberText.hpp>
#include <vine/backend_vk/internal/SampleCounts.hpp>

VN_BACKEND_VK_NS_BEGIN

using vn::graphics::planTarget;
using vn::graphics::RepairReason;
using vn::graphics::TargetAction;
using vn::graphics::TargetDecision;
using vn::graphics::TargetDesc;
using vn::graphics::TargetInstance;

namespace
{

/**
 * @brief Builds a diagnostic about ONE target: severity, category, stable code, the target's name and (when it
 *        applies) the limit it exceeded.
 *
 * @details
 * - WHY A HELPER: a target refusal is the same five facts at six sites, and the `subject` (the target's name) is what
 *   a host badges on - it must not be forgotten at one of them (see `RenderDiagnostic`).
 * - THE CODE IS THE STABLE HALF: `message` may be reworded, `code` may not (`scripts/diagnostic_codes.txt` is the
 *   registry both directions are checked against).
 *
 * @param target   The target the report is about (its name becomes the subject).
 * @param severity How bad it is.
 * @param category The coarse class.
 * @param code     The stable identifier of the cause.
 * @param message  The human sentence (with the numbers in it).
 * @param limit    The requested/supported pair, when the report is about a device limit.
 * @return The diagnostic, ready to hand to the table's report callback.
 */
[[nodiscard]] vn::graphics::RenderDiagnostic targetDiagnostic(const vn::graphics::RenderTarget&            target,
                                                             const vn::graphics::DiagnosticSeverity        severity,
                                                             const vn::graphics::DiagnosticCategory        category,
                                                             const char8_t*                                code,
                                                             vn::String                                    message,
                                                             std::optional<vn::graphics::DiagnosticLimit> limit = std::nullopt)
{
    vn::graphics::RenderDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.category = category;
    diagnostic.code     = vn::String(code);
    diagnostic.message  = std::move(message);
    diagnostic.subject  = target.name();
    diagnostic.limit    = limit;
    return diagnostic;
}

} // namespace

TargetTable::Lease::Lease() noexcept = default;

TargetTable::Lease::Lease(TargetTable& table, const FrameDriverId driver) noexcept
  : table_(&table)
  , driver_(driver)
{
}

TargetTable::Lease::Lease(Lease&& other) noexcept
  : table_(other.table_)
  , driver_(other.driver_)
  , held_(std::move(other.held_))
{
    other.table_  = nullptr;
    other.driver_ = 0;
}

TargetTable::Lease& TargetTable::Lease::operator=(Lease&& other) noexcept
{
    if (this != &other) {
        releaseAll();
        table_        = other.table_;
        driver_       = other.driver_;
        held_         = std::move(other.held_);
        other.table_  = nullptr;
        other.driver_ = 0;
    }
    return *this;
}

TargetTable::Lease::~Lease()
{
    // P3'S ONE RELEASE POINT: a frame's many early returns all end here, so no path can keep a target claimed past its
    // frame - the one that does would hold it until the process exits, with the other driver skipping every pass.
    releaseAll();
}

bool TargetTable::Lease::claim(const Target target)
{
    if (table_ == nullptr || !table_->tryClaim(target, driver_)) {
        return false;
    }
    if (std::find(held_.begin(), held_.end(), target) == held_.end()) {
        held_.push_back(target);
    }
    return true;
}

void TargetTable::Lease::release(const Target target) noexcept
{
    if (table_ == nullptr) {
        return;
    }
    table_->releaseOne(target, driver_);
    held_.erase(std::remove(held_.begin(), held_.end(), target), held_.end());
}

std::size_t TargetTable::Lease::size() const noexcept
{
    return held_.size();
}

TargetTable::Lease::operator bool() const noexcept
{
    return !held_.empty();
}

void TargetTable::Lease::releaseAll() noexcept
{
    if (table_ == nullptr) {
        return;
    }
    for (const Target target : held_) {
        table_->releaseOne(target, driver_);
    }
    held_.clear();
    table_  = nullptr;
    driver_ = 0;
}

TargetTable::~TargetTable()
{
    // The books die with the object. The RECORDS are not destroyed here: destroying Vulkan objects needs a device and
    // a pool, so teardown is `drain()` (the device idles, then calls destroyTarget on each record) - a destructor that
    // silently dropped them would leak every image of a session that forgot to.
    holders_.clear();
}

void TargetTable::open(const Device device, const Limits limits, Report report, Park park)
{
    device_ = device;
    limits_ = limits;
    report_ = std::move(report);
    park_   = std::move(park);
    // A NEW SESSION SAYS NOTHING IT SAID BEFORE: the six refusals are episodes of THIS session's build.
    size_unknown_report_.rearm();
    samples_report_.rearm();
    samples_depth_report_.rearm();
    borrow_samples_report_.rearm();
    borrow_source_report_.rearm();
    borrow_extent_report_.rearm();
}

void TargetTable::close() noexcept
{
    device_ = Device{};
    limits_ = Limits{};
    report_ = {};
    park_   = {};
    const std::lock_guard<std::mutex> claims_held(claims_mutex_);
    holders_.clear();
}

bool TargetTable::ensure(const vn::graphics::RenderTarget& target)
{
    // ONE TARGET MAP OPERATION AT A TIME: two frame heads may build (or find) different targets on two threads, and a
    // host verb may be releasing one under the exclusive gate (S6c-2).
    const std::lock_guard<std::mutex> map_held(map_mutex_);
    if (device_.table == nullptr || device_.allocator == nullptr || device_.device == VK_NULL_HANDLE) {
        // NO SESSION: nothing can be built. A frame never reaches here (its driver checks `initialized()` first), so
        // this is the answer a caller gets instead of a build against null handles.
        return false;
    }
    // WHAT THE HOST'S OBJECT ASKS FOR, in the value `planTarget` compares: an extent plus the attachment-set
    // compatibility - the engine's formats AND the device spelling only this backend can fill (see
    // RenderPassCompatibility).
    vn::graphics::TargetDesc wanted;
    wanted.width  = target.width();
    wanted.height = target.height();
    for (int index = 0; index < target.colorCount(); ++index) {
        wanted.shape.color_formats.push_back(target.colorFormat(index));
        wanted.shape.device_color_formats.push_back(
            vn::graphics::DeviceFormat{ static_cast<std::uint32_t>(colorFormatOf(target.colorFormat(index))) });
    }
    if (target.hasDepth()) {
        wanted.shape.depth_format        = target.depthFormat();
        wanted.shape.device_depth_format = vn::graphics::DeviceFormat{ static_cast<std::uint32_t>(depthFormatOf(target.depthFormat())) };
    }
    if (wanted.width > 0 && wanted.height > 0) {
        // A USABLE EXTENT EXISTS: the "unusable extent" episode is over (see ReportOnce). It is a different fact from
        // the sample-count refusals below, so it has an episode of its own.
        size_unknown_report_.rearm();
    }
    // A COUNT IS A COUNT (see ADR-2026-11-14-sample-count-refused-not-lowered): the target states the number of
    // samples it is drawn with, and `wanted.shape.samples` is that number - never another one. A count this device
    // cannot build (above its limit, or not a power of two) REFUSES the target: one warning naming the request and
    // the limit, nothing created, nothing drawn into it, and `readColorBuffer` answering NotReady. Lowering it would
    // leave `RenderTarget::samples()` saying a number the images are not drawn with, which no value in this shape can
    // express.
    const std::uint32_t requested_samples = target.samples() == 0U ? 1U : target.samples();
    const std::uint32_t largest_samples   = largestSampleCount(limits_.supported_sample_counts);
    if (!ensureBuildableSamples(requested_samples, limits_.supported_sample_counts)) {
        if (samples_report_.shouldReport()) {
            // The two reasons a count is unbuildable are said as the difference they are: a request past the limit,
            // and a number that is not a sample count at all. Both carry the pair a host acts on (requested vs
            // offered) and their own code, so the two are distinguishable without reading the sentence.
            const vn::String request = vn::String(u8"vk: a target asks for ") + numberText(requested_samples) + u8" samples per pixel";
            const bool       above   = requested_samples > largest_samples;
            const vn::String why     = above
                                           ? request + u8", above what this device offers (up to " + numberText(largest_samples) +
                                                 u8"; see Capabilities::limits.max_samples)"
                                           : request + u8", which is not a sample count this device offers (the counts are powers of two, up to " +
                                                 numberText(largest_samples) + u8"; see Capabilities::limits.max_samples)";
            if (report_) {
                report_(targetDiagnostic(target,
                                         vn::graphics::DiagnosticSeverity::Warning,
                                         vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                         above ? u8"vk.target.samples_above_limit" : u8"vk.target.samples_not_a_count",
                                         why,
                                         vn::graphics::DiagnosticLimit{ requested_samples, largest_samples }));
            }
        }
        return false;
    }
    samples_report_.rearm(); // a target with a count this device builds: the episode is over
    wanted.shape.samples         = requested_samples;
    wanted.shape.render_pass_key = kSingleSubpassRenderPass;
    // A MULTISAMPLED DEPTH IS RESOLVED, and a device that cannot do that refuses the combination (ADR-2026-11-14).
    // `TargetObjects` builds the single-sample picture the rendering instance resolves into and every reader uses
    // (`depthPicture`: the shadow path samples it), with `sample0` as the mode - so multisampling cannot change the
    // depth a later pass tests against. The resolve is Vulkan 1.2 (see Limits::depth_resolve_supported), and a device
    // below that genuinely cannot serve this shape: nothing is created, nothing is drawn, and `readColorBuffer`
    // answers NotReady.
    if (requested_samples > 1U && target.hasDepth() && !limits_.depth_resolve_supported) {
        if (samples_depth_report_.shouldReport()) {
            if (report_) {
                report_(targetDiagnostic(target,
                                         vn::graphics::DiagnosticSeverity::Warning,
                                         vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                         u8"vk.target.multisampled_depth_unsupported",
                                         vn::String(u8"vk: a target asks for ") + numberText(requested_samples) +
                                             u8" samples per pixel and a depth attachment; this device cannot resolve a multisampled depth "
                                             u8"(it needs Vulkan 1.2's depth/stencil resolve), so the target is refused (see Capabilities::limits.max_samples)",
                                         vn::graphics::DiagnosticLimit{ requested_samples, 1U }));
            }
        }
        return false;
    }
    samples_depth_report_.rearm(); // a depth-attached target this device can resolve: the episode is over

    // A BORROWED DEPTH (RenderTarget::shareDepth): this target draws against ANOTHER target's depth image instead of
    // owning one, which is what makes "the composite tests against the G-buffer's depth" a composition rather than
    // two copies of one picture. Three facts are the host's to satisfy, and each refusal is its own episode because
    // each has a different repair:
    //  - a MULTISAMPLED target cannot borrow: one image cannot be a multi-sample attachment for one pass and a
    //    single-sample picture for another (the SDK says every backend refuses that combination);
    //  - the LENDER is a target THIS device has built and that has a depth - the SDK's ordering rule (the lender
    //    renders earlier in the frame) is what makes that true, and a frame that breaks it is wiring, not a capability;
    //  - both targets are EQUALLY SIZED: the lender's picture is bound into a framebuffer of the borrower's extent.
    const vn::graphics::RenderTarget* lender      = target.depthSource();
    VkImageView                       lender_view = VK_NULL_HANDLE;
    if (lender != nullptr) {
        if (requested_samples > 1U) {
            if (borrow_samples_report_.shouldReport()) {
                if (report_) {
                    report_(targetDiagnostic(target,
                                             vn::graphics::DiagnosticSeverity::Warning,
                                             vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                             u8"vk.target.borrow_multisampled",
                                             vn::String(u8"vk: a target asks for ") + numberText(requested_samples) +
                                                 u8" samples per pixel and borrows its depth (RenderTarget::shareDepth); one depth image "
                                                 u8"cannot be a multi-sample attachment for one pass and a single-sample picture for another",
                                             vn::graphics::DiagnosticLimit{ requested_samples, 1U }));
                }
            }
            return false;
        }
        borrow_samples_report_.rearm(); // a single-sample borrower: the episode is over
        const auto source = targets_.find(lender);
        if (source == targets_.end() || !source->second->built() || !source->second->hasDepth()) {
            if (borrow_source_report_.shouldReport()) {
                if (report_) {
                    report_(targetDiagnostic(target,
                                             vn::graphics::DiagnosticSeverity::Warning,
                                             vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                             u8"vk.target.borrow_source_missing",
                                             u8"vk: a target borrows its depth from a target this device has not built (or one with no "
                                             u8"depth attachment); the lender must render earlier in the same frame (see RenderTarget::shareDepth)"));
                }
            }
            return false;
        }
        borrow_source_report_.rearm(); // the lender is built here: the episode is over
        if (lender->width() != target.width() || lender->height() != target.height()) {
            if (borrow_extent_report_.shouldReport()) {
                if (report_) {
                    report_(targetDiagnostic(target,
                                             vn::graphics::DiagnosticSeverity::Warning,
                                             vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                             u8"vk.target.borrow_extent_mismatch",
                                             vn::String(u8"vk: a target borrows the depth of a differently sized target (") +
                                                 numberText(static_cast<std::uint32_t>(lender->width())) + u8"x" +
                                                 numberText(static_cast<std::uint32_t>(lender->height())) + u8" against " +
                                                 numberText(static_cast<std::uint32_t>(target.width())) + u8"x" +
                                                 numberText(static_cast<std::uint32_t>(target.height())) + u8"); a shared depth has one extent"));
                }
            }
            return false;
        }
        borrow_extent_report_.rearm(); // equally sized: the episode is over
        lender_view = source->second->depthPictureView();
        // THE LENDER'S DEPTH FORMAT IS WHAT THIS TARGET'S PASS IS BUILT WITH: the borrower's own declaration is not a
        // picture anyone has, and the shape states what the attachment IS - so a lender rebuilt at another format
        // rebuilds the borrower through `planTarget`'s own comparison.
        wanted.shape.depth_format        = source->second->desc.shape.depth_format;
        wanted.shape.device_depth_format = source->second->desc.shape.device_depth_format;
    }

    const auto [entry, inserted] = targets_.try_emplace(&target);
    (void)inserted;
    if (entry->second == nullptr) {
        entry->second = std::make_shared<TargetObjects>();
    }
    TargetObjects& images = *entry->second;
    TargetInstance current;
    current.desc                    = images.desc;
    current.generation              = 0U;
    current.built                   = images.built();
    current.attachments_invalidated = false;

    TargetDecision decision = planTarget(current, wanted);
    // THE BORROW EDGE IS PART OF WHAT WAS BUILT even though `planTarget` does not compare it: it is not a property of
    // the shape but a relationship to another record, so a target that started or stopped borrowing - or that borrows
    // a lender whose depth picture was rebuilt - has a different framebuffer to bind. A target with no usable extent
    // still decides first, as always.
    const bool borrow_moved = images.built() && (images.borrowsDepth() != (lender != nullptr) ||
                                                 (lender != nullptr && images.borrowed_depth_view != lender_view));
    if (borrow_moved && !(decision.action == TargetAction::Repair && decision.reason == RepairReason::SizeUnknown)) {
        decision.action = TargetAction::Rebuild;
        decision.reason = RepairReason::None;
    }
    switch (decision.action) {
    case TargetAction::None:
        return true;
    case TargetAction::Repair:
        if (decision.reason == RepairReason::SizeUnknown) {
            if (size_unknown_report_.shouldReport()) {
                if (report_) {
                    report_(targetDiagnostic(target,
                                             vn::graphics::DiagnosticSeverity::Warning,
                                             vn::graphics::DiagnosticCategory::TargetBuildFailed,
                                             u8"vk.target.size_unknown",
                                             u8"vk: a target with an unusable extent (0 in a dimension) was left unbuilt"));
                }
            }
            return false;
        }
        // Bootstrap: the attachments exist and have the right shape; the colour attachments always CLEAR, so the first
        // pass in does not need a second variant.
        return true;
    case TargetAction::ResizeInPlace:
        images.desc = wanted;
        if (!resizeTarget(*device_.table, device_.device, *device_.allocator, images)) {
            if (report_) {
                report_(targetDiagnostic(target,
                                         vn::graphics::DiagnosticSeverity::Error,
                                         vn::graphics::DiagnosticCategory::TargetBuildFailed,
                                         u8"vk.target.resize_failed",
                                         u8"vk: the target's attachments could not be rebuilt at the new extent (see the memory report)"));
            }
            return false;
        }
        return true;
    case TargetAction::Rebuild:
        // The shape changed: the attachments are rebuilt; pipelines are keyed by compatibility, so the ones built for
        // the old shape are simply never asked for again (and a resize, which keeps the shape, reuses them - see
        // ContentCaches).
        destroyTarget(*device_.table, device_.device, *device_.allocator, images);
        images.desc                = wanted;
        images.depth_borrowed      = lender != nullptr;
        images.borrowed_depth_view = lender_view;
        if (!buildTarget(*device_.table, device_.device, *device_.allocator, images)) {
            if (report_) {
                report_(targetDiagnostic(target,
                                         vn::graphics::DiagnosticSeverity::Error,
                                         vn::graphics::DiagnosticCategory::TargetBuildFailed,
                                         u8"vk.target.build_failed",
                                         u8"vk: the off-screen target's attachments could not be built (see the memory report)"));
            }
            return false;
        }
        return true;
    }
    return false;
}

std::shared_ptr<TargetObjects> TargetTable::lookup(const Target target) const noexcept
{
    const std::lock_guard<std::mutex> map_held(map_mutex_);
    const auto                        found = targets_.find(target);
    if (found == targets_.end()) {
        return nullptr;
    }
    return found->second;
}

TargetTable::Peek TargetTable::peek(const Target target) const
{
    // THE LOCK EVERY WRITER TAKES: `ensure` holds it across a whole build or resize, and `release` / `drain` erase
    // under it - so what is read here is ONE state, never a half-rebuilt record, and never a record the table has
    // already handed to the park (the map is how a record is found at all). `fresh` is the one field a frame body
    // writes without this lock; it is atomic, and a value one pass old is a legal answer (see Peek).
    const std::lock_guard<std::mutex> map_held(map_mutex_);
    const auto                        found = targets_.find(target);
    if (found == targets_.end() || found->second == nullptr || !found->second->built()) {
        return Peek{};
    }
    const TargetObjects& images = *found->second;
    Peek                 peek;
    peek.built = true;
    peek.fresh = images.fresh.load(std::memory_order_relaxed);
    peek.shape = images.desc.shape;
    return peek;
}

bool TargetTable::release(const Target target)
{
    std::shared_ptr<TargetObjects> doomed;
    {
        const std::lock_guard<std::mutex> map_held(map_mutex_);
        const auto                        found = targets_.find(target);
        if (found == targets_.end()) {
            return false; // never built, or already released: the host's statement is already true
        }
        doomed = std::move(found->second);
        targets_.erase(found);
    }
    if (doomed != nullptr) {
        if (park_) {
            park_(std::move(doomed));
        }
        // NO PARK: the table has no clock to date the release against (a device being torn down drains first), so the
        // record is simply forgotten - the same answer `destroyTarget` gives an empty record.
    }
    return true;
}

std::vector<std::shared_ptr<TargetObjects>> TargetTable::drain() noexcept
{
    std::vector<std::shared_ptr<TargetObjects>> out;
    {
        const std::lock_guard<std::mutex> map_held(map_mutex_);
        out.reserve(targets_.size());
        for (auto& [identity, images] : targets_) {
            (void)identity;
            out.push_back(std::move(images));
        }
        targets_.clear();
    }
    {
        const std::lock_guard<std::mutex> claims_held(claims_mutex_);
        holders_.clear();
    }
    return out;
}

TargetTable::Lease TargetTable::lease(const FrameDriverId driver)
{
    return Lease{ *this, driver };
}

std::optional<TargetTable::FrameDriverId> TargetTable::holderOf(const Target target) const
{
    const std::lock_guard<std::mutex> claims_held(claims_mutex_);
    const auto                        found = holders_.find(target);
    if (found == holders_.end()) {
        return std::nullopt;
    }
    return found->second;
}

TargetTable::Numbers TargetTable::claimNumbers() const noexcept
{
    const std::lock_guard<std::mutex> claims_held(claims_mutex_);
    Numbers                           out = numbers_;
    out.live                              = holders_.size();
    return out;
}

bool TargetTable::tryClaim(const Target target, const FrameDriverId driver) noexcept
{
    const std::lock_guard<std::mutex> claims_held(claims_mutex_);
    const auto                        found = holders_.find(target);
    if (found == holders_.end()) {
        holders_.emplace(target, driver);
        ++numbers_.claimed;
        return true;
    }
    if (found->second == driver) {
        // THE HOLDER ASKING AGAIN: one driver, one lease (see the class note); a repeat is not a new claim.
        return true;
    }
    ++numbers_.refused;
    return false;
}

void TargetTable::releaseOne(const Target target, const FrameDriverId driver) noexcept
{
    const std::lock_guard<std::mutex> claims_held(claims_mutex_);
    const auto                        found = holders_.find(target);
    if (found == holders_.end() || found->second != driver) {
        return; // not this driver's claim (already released, or cleared at teardown): nothing to do
    }
    holders_.erase(found);
    ++numbers_.released;
}

VN_BACKEND_VK_NS_END
