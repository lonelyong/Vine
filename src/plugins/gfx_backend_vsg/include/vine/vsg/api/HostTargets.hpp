#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <vsg/core/ref_ptr.h>
#include <vsg/vk/Device.h>

#include <vine/graphics/RenderTarget.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The HOST's off-screen targets, as this backend holds them: the description it copied, the GPU
 * objects a frame draws into, and the facts the plan is compiled from.
 *
 * WHY A LAYER OF ITS OWN. The SDK's `RenderTarget` is a description and nothing else ("the backend owns the
 * attachments; the RenderTarget stays a logical description"), while api/OffscreenTarget owns one set of GPU
 * objects for one shape. Between them sit three facts nobody else can hold:
 *
 *   * the DESCRIPTION is COPIED, never borrowed. The SDK's contract says a pointer argument is valid for the
 *     duration of the call and that the host may destroy the object once it announced the removal
 *     (`releaseRenderTarget`), so what is kept here is a snapshot - refreshed by every call that names the
 *     target - and no host object is dereferenced between calls.
 *   * the BUILD is LAZY (`ensure`): the objects are created the first time the description can make a target
 *     (a positive extent and at least one colour attachment - the host configures a target before it draws
 *     into it), and the resize / rebuild after that is the PLAN's answer, applied by the executor (the plan
 *     is what decides, see core::planTarget; this type never rebuilds on its own).
 *   * the FACTS (`facts`) are what the compiler resolves the frame's targets AND inputs from: the wanted
 *     description, what the attachments currently are, the depth's promotion / borrowing and the shadow
 *     statement - one row per target, and a row cannot disagree with the object it describes because both
 *     come from here.
 *
 * LIFETIME. An entry owns its OffscreenTarget through a shared_ptr, and a target that BORROWS another's depth
 * holds a share of the lender's: releasing the lender (the SDK announces that the host may destroy it now)
 * drops the registry's entry while a borrower keeps the image alive - the SDK's own shareDepth keeps the
 * source alive on its side, and this is the matching rule on ours.
 *
 * WHAT IT DOES NOT HOLD: the window (its own type, see WindowTarget) and anything about what a pass DOES with
 * a target - the clear policy, the load ops, the scheduling. Those are the plan's, and a target that started
 * answering them would be the second backend.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
VN_VSG_NS_BEGIN

/** @brief The host's off-screen targets and the facts the frame is compiled from (see the file note). */
class VN_VSG_API HostTargets
{
  public:
    /** @brief The SDK's description of one target, copied (see the file note). */
    struct Description
    {
        std::int32_t                                           width{0};         ///< Extent in pixels.
        std::int32_t                                           height{0};
        std::vector<vn::graphics::RenderTarget::ColorFormat> color_formats;   ///< One per colour attachment.
        bool                                                   has_depth{false};   ///< Own or borrowed depth.
        vn::graphics::RenderTarget::DepthFormat depth_format{vn::graphics::RenderTarget::DepthFormat::D24};
        bool        depth_promotion{true};      ///< The host asked for a sampleable depth (own depth only).
        const void* depth_source{nullptr};      ///< The lender, when the depth is borrowed (its identity).
        const void* shadow_light{nullptr};      ///< The light this target is the shadow map of, when stated.
        bool        has_view_projection{false}; ///< Whether the producer stated how to read the map.
        vn::math::Mat4d view_projection{};    ///< The producer's projection * view, when stated.
    };

    /** @brief One host target: the copied description, its objects, and the lender's when its depth is borrowed. */
    struct Entry
    {
        const void*                      identity{nullptr};  ///< The SDK object this entry stands for.
        Description                      description{};     ///< The snapshot last observed from the host.
        std::shared_ptr<OffscreenTarget> target{};          ///< The GPU objects, once built.
        std::shared_ptr<OffscreenTarget> depth_owner{};     ///< The lender's objects, while its depth is reused.
        core::ReportOnce                 report{};          ///< One episode per entry (the caller decides what to say).
        core::ReportOnce                 readback_report{}; ///< A readback refusal's own episode (re-armed by success).
    };

    /** @brief What ensuring an entry did (the caller owns the diagnostic stream and the sentences). */
    enum class State : std::uint8_t
    {
        Ready,              ///< The entry has objects for its description (or already had them).
        NotBuilt,           ///< The description cannot make a target yet: no positive extent / no colour.
        DepthSourceMissing, ///< It borrows a depth from a target this backend does not hold (or has not built).
        BuildFailed,        ///< The description is complete but the objects could not be created.
    };

    /** @brief The result of @ref ensure. */
    struct Ensured
    {
        Entry* entry{nullptr};      ///< The entry (never null).
        State  state{State::Ready}; ///< What happened (see State).
    };

  public:
    HostTargets() = default;

    HostTargets(const HostTargets&)            = delete;
    HostTargets& operator=(const HostTargets&) = delete;

    /** @brief Copies @p target's description into its entry (creating the entry) and builds what it asks for.
     *
     * The copy happens on every call and only carries what changed (the comparison is field by field), so a
     * host that announces the same target every frame allocates nothing.
     *
     * @param target The host's target; borrowed for the call - only its description is kept.
     * @param device The device the objects belong to, or null while none is up (nothing is built then, and
     *               the description is still kept - the next call with a device builds it).
     * @return The entry, and what happened (see State).
     */
    [[nodiscard]] Ensured ensure(const vn::graphics::RenderTarget& target, ::vsg::ref_ptr<::vsg::Device> device);

    /** @brief Copies @p target's description into its entry without building anything.
     *
     * For the targets a frame only READS (its pass inputs): their objects were built when they were drawn
     * into, and what this call keeps current is the description the facts are made of.
     *
     * @param target The host's target; borrowed for the call.
     * @return The entry (never null).
     */
    [[nodiscard]] Entry* observe(const vn::graphics::RenderTarget& target);

    /** @brief Gets the entry @p identity stands for, or null when this backend does not hold it. */
    [[nodiscard]] Entry* find(const void* identity) noexcept;

    /** @brief Forgets @p identity: the SDK's announcement that the host may destroy the target now.
     *
     * The objects go with the entry - unless a borrower holds a share of them (a borrowed depth's owner),
     * which is the one reference that must outlive the announcement.
     *
     * @param identity Target going away (null is ignored).
     * @return true when it was held, false when it was not.
     */
    [[nodiscard]] bool release(const void* identity) noexcept;

    /** @brief Drops every entry: the session is going down, and the objects belong to its device. */
    void clear() noexcept;

    /** @brief Gets how many targets are held. */
    [[nodiscard]] std::size_t live() const noexcept;

    /** @brief Gets the entries, in first-announced order (the facts table's order). */
    [[nodiscard]] const std::vector<std::unique_ptr<Entry>>& entries() const noexcept;

    /** @brief Writes the facts row the compiler resolves @p entry from (see the file note).
     *
     * @param entry The entry to describe.
     * @param out   Receives the row: its identity, the wanted description, what the objects currently are,
     *              the depth's promotion / borrowing and the shadow statement.
     */
    void facts(const Entry& entry, core::TargetFacts& out) const;

  private:
    /** @brief Copies what changed from @p target into @p out. */
    static void describe(const vn::graphics::RenderTarget& target, Description& out);

    /** @brief Builds @p entry's objects when it has none and its description can make them. */
    [[nodiscard]] static State build(Entry& entry, ::vsg::ref_ptr<::vsg::Device> device);

  private:
    std::vector<std::unique_ptr<Entry>> entries_;  ///< The held targets, in first-announced order.
};

VN_VSG_NS_END
