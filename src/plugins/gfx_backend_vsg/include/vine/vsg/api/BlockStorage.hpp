#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/Buffer.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/core/FrameRing.hpp>
#include <vine/vsg/core/MaterialArena.hpp>
#include <vine/vsg/core/SlotProbe.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The bytes every frame writes: one mapped buffer holding the view blocks, the draw blocks and the
 * material blocks, with the offsets coming from the core's ring and arena.
 *
 * WHY ONE BUFFER AND WHY MAPPED. These blocks change while frames are in flight, so writing them means
 * writing into memory the GPU is reading. A `vsg::Data` marked DYNAMIC would transfer a whole array every
 * time one value changed; host-visible, host-coherent memory lets the frame's own thread write the block in
 * place, with visibility guaranteed and no staging copy - the same choice the existing draw-block pool made,
 * and the reason a steady frame transfers nothing.
 *
 * WHAT DECIDES THE OFFSETS. Nothing here does: the ring (`core::FrameRing`) says which slab this frame may
 * write, and the material arena (`core::MaterialArena`) says which slot and which in-flight copy a material's
 * bytes live in. This class only turns those decisions into byte offsets in one buffer and performs the
 * copy, which is why its interesting properties (rotation, steady state, bounded growth) are already pinned
 * by device-free cases.
 *
 * THE ALIGNMENT IS THE DEVICE'S. A dynamic offset must be a multiple of
 * `minUniformBufferOffsetAlignment`, and only the device knows that number, so this class reads it and lays
 * every region out with it. Callers state a block's SIZE; they never state an offset or a stride.
 *
 * WHAT IT REFUSES, AND WHAT THAT ASKS FOR. A block larger than its region's stride is refused outright and
 * counted (a caller error, never a reason to grow). A frame that asks past its region's budget is refused
 * TOO - and that refusal stands FOR THAT FRAME, because growing the buffer in place would move bytes a
 * submitted command buffer still names. What the frame tried is remembered instead (`growthNeeded()`), and
 * the caller replaces the whole storage between frames: a new buffer with larger budgets, the old one parked
 * through the retirement queue until no command buffer can still name it (the facade does exactly that, see
 * VsgBackend::growBlockStorageIfNeeded). The storage itself never grows - the same rule its rings state.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
VN_VSG_NS_BEGIN

/** @brief The frame's mapped block storage (views, draws, lights, shadows, materials). */
class BlockStorage
{
  public:
    /** @brief The storage's shape. Alignment fields are not read - the device's own alignment is used. */
    struct Layout
    {
        /// The slabs every ring below owns on a session whose in-flight count has not been learned yet: one
        /// MORE than that count, because a frame writes the slab the oldest frame still allowed to be in
        /// flight reads (see core::perFrameCopies - getting this wrong is not a lost frame but a wrong one).
        static constexpr std::uint32_t kAssumedSlabs = core::perFrameCopies(core::kAssumedInFlightSlots);

        core::FrameRing::Layout     views{ 288U, 1U, kAssumedSlabs, 256U };   ///< View blocks (one per pass per frame).
        core::FrameRing::Layout     draws{ 80U, 1U, kAssumedSlabs, 1024U };   ///< Draw blocks (one per draw per frame).
        core::FrameRing::Layout     lights{ 112U, 1U, kAssumedSlabs, 1024U }; ///< Light blocks (one per drawing call).
        core::FrameRing::Layout     shadows{ 80U, 1U, kAssumedSlabs, 1024U }; ///< Shadow blocks (one per drawing call).
        core::MaterialArena::Layout materials{ 64U, kAssumedSlabs, 256U };    ///< Material blocks (persistent, rotating).
    };

    /** @brief Where a block was written. */
    struct Block
    {
        bool          valid{false};  ///< false when the write was refused (see the file note).
        std::uint64_t offset{0};     ///< Byte offset in the storage's buffer - the dynamic offset to bind.
    };

    /** @brief What a material write did, and where the bytes are. */
    struct MaterialWrite
    {
        core::MaterialArena::WriteKind kind{core::MaterialArena::WriteKind::Unchanged};  ///< What happened.
        /// Byte offset in the storage's buffer - the dynamic offset to bind, meaningful for EVERY answer: a
        /// hit names the block the last write left, which is exactly what a draw must bind (its bytes are the
        /// material's - see `writeMaterial`).
        std::uint64_t                  offset{0};
        std::uint64_t                  bytes{0};   ///< Bytes written (0 when nothing was).
    };

    /** @brief The regions the buffer is divided into (what a write's offset must fall inside). */
    struct Regions
    {
        std::uint64_t views_base{0};      ///< Start of the view-block region.
        std::uint64_t views_bytes{0};     ///< Size of the view-block region.
        std::uint64_t draws_base{0};      ///< Start of the draw-block region.
        std::uint64_t draws_bytes{0};     ///< Size of the draw-block region.
        std::uint64_t lights_base{0};     ///< Start of the light-block region.
        std::uint64_t lights_bytes{0};    ///< Size of the light-block region.
        std::uint64_t shadows_base{0};    ///< Start of the shadow-block region.
        std::uint64_t shadows_bytes{0};   ///< Size of the shadow-block region.
        std::uint64_t materials_base{0};  ///< Start of the material-block region.
        std::uint64_t materials_bytes{0}; ///< Size of the material-block region.
    };

    /** @brief The distance between two blocks of one region, including the device's alignment padding. */
    struct Strides
    {
        std::uint64_t view{0};      ///< Bytes between two view blocks.
        std::uint64_t draw{0};      ///< Bytes between two draw blocks.
        std::uint64_t light{0};     ///< Bytes between two light blocks.
        std::uint64_t shadow{0};    ///< Bytes between two shadow blocks.
        std::uint64_t material{0};  ///< Bytes between two material blocks.
    };

    /** @brief What the frames written so far tried to write where a budget refused them (the growth request).
     *
     * One field per ring region: the blocks the WORST frame since the storage was built tried to write
     * (written or refused), or 0 when that region never ran out of budget. A region at 0 must not grow - a
     * scene that only ever filled its draw budget must not pay for bigger view blocks.
     */
    struct Growth
    {
        std::uint32_t views{0};    ///< Blocks the worst frame tried to write into the view region.
        std::uint32_t draws{0};    ///< Blocks the worst frame tried to write into the draw region.
        std::uint32_t lights{0};   ///< Blocks the worst frame tried to write into the light region.
        std::uint32_t shadows{0};  ///< Blocks the worst frame tried to write into the shadow region.

        /** @brief Whether any region ran out of budget (only then is a replacement storage needed). */
        [[nodiscard]] bool needed() const noexcept
        {
            return views != 0U || draws != 0U || lights != 0U || shadows != 0U;
        }
    };

  public:
    /** @brief Creates a storage on a device.
     *
     * @p layout is raised to the in-flight floor first (see @ref layoutForInFlight): rings that state fewer
     * slabs than the assumed in-flight count are not a smaller budget but a steady frame that overwrites
     * the slab the oldest frame in flight is still reading, so they cannot be built. A layout that already
     * serves the assumed count is built exactly as stated. The answer is the floor, not a refusal: a refusal
     * has no channel to explain itself, and a storage too shallow has no correct way to serve.
     *
     * @param device The device every buffer and mapping belongs to (the storage does not own it).
     * @param layout Block sizes, per-frame budgets and in-flight copies (raised to the floor when shallow).
     * @return The storage, or null when there is no device or the buffer/mapping could not be created.
     */
    static std::unique_ptr<BlockStorage> create(::vsg::ref_ptr<::vsg::Device> device, const Layout& layout);

    ~BlockStorage();

    BlockStorage(const BlockStorage&) = delete;
    BlockStorage& operator=(const BlockStorage&) = delete;

  public:
    /** @brief Begins the next frame: rotates the rings' slabs and the arena's copy.
     *
     * The first call begins frame 0. A frame that writes blocks without calling this writes into the previous
     * frame's slab, which the device may still be reading - so this is the caller's first act of the frame.
     */
    void beginFrame() noexcept;

    /** @brief Writes one view block into this frame's slab.
     *
     * @param block Block bytes (a `VineViewBlock`).
     * @return Where it went, or `valid == false` when the block is oversized or the frame's budget ran out.
     */
    [[nodiscard]] Block writeView(std::span<const std::byte> block) noexcept;

    /** @brief Writes one draw block into this frame's slab.
     *
     * @param block Block bytes (a `VineDrawBlock`: matrix + opacity).
     * @return Where it went, or `valid == false` when the block is oversized or the frame's budget ran out.
     */
    [[nodiscard]] Block writeDraw(std::span<const std::byte> block) noexcept;

    /** @brief Writes one light block into this frame's slab.
     *
     * @param block Block bytes (a `VineLightsBlock`: one ambient plus up to three directional lights).
     * @return Where it went, or `valid == false` when the block is oversized or the frame's budget ran out.
     */
    [[nodiscard]] Block writeLights(std::span<const std::byte> block) noexcept;

    /** @brief Writes one shadow block into this frame's slab.
     *
     * @param block Block bytes (a `VineShadowBlock`: the view -> light clip matrix and the shading's scalars).
     * @return Where it went, or `valid == false` when the block is oversized or the frame's budget ran out.
     */
    [[nodiscard]] Block writeShadows(std::span<const std::byte> block) noexcept;

    /** @brief Notes a material's bytes for this frame, writing them only when the revision moved.
     *
     * @param material Material identity (a raw pointer: the arena stores no reference to it).
     * @param revision Upstream revision of the material's bytes.
     * @param block    Block bytes (a `VineMaterialBlock`); not written when the revision matches.
     * @return What happened and where the block is; the offset is valid for every answer, so a caller binds
     *         it whether the bytes were written or were already right (see @ref MaterialWrite).
     */
    [[nodiscard]] MaterialWrite writeMaterial(const void* material, std::uint64_t revision,
                                              std::span<const std::byte> block);

  public:
    /** @brief Gets the regions the buffer is divided into. */
    [[nodiscard]] Regions regions() const noexcept;

    /** @brief Gets the strides between two blocks of each region (see @ref Strides). */
    [[nodiscard]] Strides strides() const noexcept;

    /** @brief Gets the mapping as bytes (read-only; the evidence path and the tests read it back). */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

    /** @brief Gets the buffer every region lives in (what the descriptor layer binds). */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::Buffer> buffer() const noexcept;

    /** @brief Gets the size of the buffer, in bytes. */
    [[nodiscard]] std::uint64_t capacityBytes() const noexcept;

    /** @brief Gets the layout this storage was built with (what a growth doubles from).
     *
     * The layout as it was BUILT - block sizes and budgets as the caller stated them, with the rings already
     * raised to the in-flight floor (see @ref create) - and not the device-adjusted strides: this is what
     * @ref grownLayout takes (see VsgBackend::growBlockStorageIfNeeded), and it is how a caller answers
     * "what is this session's budget" without keeping a second copy of the question.
     */
    [[nodiscard]] Layout layout() const noexcept;

    /** @brief Gets the number of frames begun. */
    [[nodiscard]] std::uint64_t frames() const noexcept;

    /** @brief Gets the number of blocks actually written (views, draws, lights, shadows and materials). */
    [[nodiscard]] std::uint64_t writes() const noexcept;

    /** @brief Gets the total bytes written. */
    [[nodiscard]] std::uint64_t bytesWritten() const noexcept;

    /** @brief Gets the number of writes refused because the block was larger than its stride. */
    [[nodiscard]] std::uint64_t oversized() const noexcept;

    /** @brief Gets the number of view/draw/light/shadow writes refused because the frame's budget ran out. */
    [[nodiscard]] std::uint64_t overflows() const noexcept;

    /** @brief Gets what the frames so far tried to write where a budget refused them (see @ref Growth).
     *
     * Grows as the max over frames and is deliberately NEVER reset: the request is applied by building a
     * REPLACEMENT storage and letting this one go (see @ref grownLayout), and a replacement starts with no
     * request of its own - so "clear the request" is not an operation this type has.
     */
    [[nodiscard]] Growth growthNeeded() const noexcept;

    /** @brief Gets whether @p layout owns a slab for every frame that may be in flight, plus the spare.
     *
     * Every per-frame shape is asked (views, draws, lights, shadows and the material copies), because a ring
     * that states fewer than core::perFrameCopies(@p frames_in_flight) slabs is not a smaller budget: the
     * frame being recorded writes the slab the oldest frame still allowed to be in flight reads (see
     * core::perFrameCopies). Device-free, so the policy is testable without a device.
     *
     * @param layout           The layout to check.
     * @param frames_in_flight Frames that may be in flight: the assumed count until the session learned its
     *                         own (see Session::slots), the learned count afterwards.
     * @return true when every per-frame shape owns at least core::perFrameCopies(@p frames_in_flight) slabs.
     */
    [[nodiscard]] static bool hasSlabsForInFlight(const Layout& layout, std::uint32_t frames_in_flight) noexcept;

    /** @brief Gets @p layout with every per-frame shape raised to core::perFrameCopies(@p frames_in_flight).
     *
     * The raise is the floor @ref create applies (with the assumed count) and the correction the session
     * applies once it LEARNED a deeper count than the storage was built for (see
     * VsgBackend::growBlockStorageIfNeeded). Shapes that already own enough slabs are left as they are - it
     * never shrinks, because a ring sized for a shallower count is safe exactly until the next count is
     * learned (and its pages are already paid for). Strides and budgets are not touched.
     *
     * @param layout           The layout to raise.
     * @param frames_in_flight Frames the raised layout must serve.
     * @return The raised layout (equal to @p layout when it already serves @p frames_in_flight).
     */
    [[nodiscard]] static Layout layoutForInFlight(const Layout& layout, std::uint32_t frames_in_flight) noexcept;

    /** @brief Gets the layout a replacement storage must be built with to serve @p need (doubling policy).
     *
     * Each region @p need names grows to `max(need, 2 x budget)`: the doubling amortises a scene that keeps
     * adding drawables (a few events, then none), and taking the need when it is larger keeps one big frame
     * from growing twice. Regions @p need leaves at 0 keep their budget. Device-free, so the policy is
     * testable without a device.
     *
     * @param current The layout the storage being replaced was built with.
     * @param need    What that storage's frames tried to write where they were refused.
     * @return The layout for the replacement (equal to @p current when nothing needs to grow).
     */
    [[nodiscard]] static Layout grownLayout(const Layout& current, const Growth& need) noexcept;

    /** @brief Gets the number of materials with a slot. */
    [[nodiscard]] std::size_t liveMaterials() const noexcept;

    /** @brief Gets the number of material blocks written. */
    [[nodiscard]] std::uint64_t materialWrites() const noexcept;

    /** @brief Gets the number of material notes answered without a write (the steady frame). */
    [[nodiscard]] std::uint64_t materialHits() const noexcept;

    /** @brief Gets the number of materials dropped because the arena's capacity was reached. */
    [[nodiscard]] std::uint64_t materialEvictions() const noexcept;

  private:
    struct Data;
    // Lexically after Data so the out-of-line destructor is the only place that needs the complete type.
    std::unique_ptr<Data> d;

    explicit BlockStorage(::vsg::ref_ptr<::vsg::Device> device, const Layout& layout);
};

VN_VSG_NS_END
