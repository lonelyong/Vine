#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Node.h>
#include <vsg/state/BindDescriptorSet.h>
#include <vsg/state/PushConstants.h>

#include <vine/graphics/backend/FrameCompiler.hpp>
#include <vine/graphics/backend/StateRegistry.hpp>
#include <vine/graphics/backend/VariantPool.hpp>
#include <vine/backend_vsg/internal/ContentDraw.hpp>
#include <vine/backend_vsg/backend_vsg_global.hpp>

/**
 * @brief What ONE frame's content resolved to: values, one per drawing call, that the recording consumes.
 *
 * WHY IT EXISTS (`C3`, the vk `CommandPlan` shape). A frame's content has two halves that used to be one function:
 * the DEVICE half - the facts' rows, the compiled halves, the declared sets, the stream uploads, the material
 * arena, the pipeline identities - and the RECORDING half, which turns those answers into vsg nodes. While they
 * were one function, the device's content lock had to be held across the recording, so two drivers' content phases
 * could not overlap at all. This type is the seam between them: the frame's head resolves every answer into a
 * value here (under the device's content lock, once), and the recording then reads only these values - no table,
 * no cache, no lock.
 *
 * A PLAN IS A VALUE, AND IT OWNS WHAT IT NAMES (`G4` / `G10`). Every vsg object a call names - the pipeline, the
 * descriptor binds, the vertex binds, the push blocks - is held by a `ref_ptr` here, and `halves` is a lease on
 * the producer whose recorders the calls run through (see ContentHalves::pin): another driver's head may sweep a
 * half or a declared set between this frame's resolution and its recording, and the objects this plan resolved
 * must stay alive for the recording that names them. A retirement park is NOT that guarantee - it is dated
 * against the frames that were in flight when it was made, and the frame holding a plan may not have been
 * submitted yet (see core::RetirementQueue).
 *
 * WHAT IT IS NOT. It is not the plan of the FRAME (see core::CompiledFrame - what the pass means), and it is not
 * the frame's scratch (see FrameScratch - the arena and the recorder). It is the layer between them: "what the
 * device answered for the plan's drawing calls, as one frame's values". It is reused across frames (its calls
 * keep their capacity), it belongs to ONE frame at a time, and it is not thread-safe: each driver owns one (see
 * FrameScratch).
 *
 * WHAT IT DOES DELIBERATELY NOT CARRY. The report episodes, the refusals and the diagnostics are the RESOLVER's -
 * they are facts about content and are said where the resolution happens. A `record` below therefore cannot fail
 * with a new sentence: every call in this plan already resolved, and a call that could not is simply not in it.
 */
VN_BACKEND_VSG_NS_BEGIN

/** @brief One frame's resolved content: see the file note for what crosses the seam and what it owns. */
class ContentPlan {
  public:
    /// @brief The lease a plan holds on a producer whose answers it copied (see @ref pin).
    using Pin = std::shared_ptr<const void>;

    /// @brief The most block sets one call binds (the five roles' bound, see BlockDescriptors).
    static constexpr std::size_t kMaxBlockSets = 8U;
    /// @brief The most push ranges one call writes (the engine declares one, see ProgramAbi).
    static constexpr std::size_t kMaxPushRanges = 4U;
    /// @brief The most vertex streams one call binds (see api/GeometryFacts).
    static constexpr std::size_t kMaxChannels = 8U;

    /** @brief One drawing call, resolved: everything recording it reads, and nothing recording it would ask. */
    struct Call {
        /// The compiled half's recorder. Not const: recording counts into it (its counters are atomics, see
        /// api/ContentDraw - two drivers' frames may record through one half at once).
        ContentDraw* draws{ nullptr };
        bool         screen{ false }; // Which of the two drawing calls this is.

        /// The CONTENT call's resolved value (its spans are filled by @ref value, never stored).
        ContentDraw::Draw draw{};
        /// The FULL-SCREEN call's resolved value.
        ContentDraw::ScreenDraw screen_draw{};

        /// The block binds, one per set the program declares (kMaxBlockSets at most).
        std::array<::vsg::ref_ptr<::vsg::BindDescriptorSet>, kMaxBlockSets> blocks{};
        std::size_t                                                        block_count{ 0 };
        /// The push blocks, one per range the program declares.
        std::array<::vsg::ref_ptr<::vsg::PushConstants>, kMaxPushRanges> pushes{};
        std::size_t                                                      push_count{ 0 };
        /// The vertex streams, one per channel (then the instance stream, when the call reads one).
        std::array<::vsg::ref_ptr<::vsg::BindVertexBuffers>, kMaxChannels> vertex_binds{};
        std::size_t                                                        vertex_bind_count{ 0 };

        /** @brief Releases what this call holds and forgets its kind; the arrays keep their storage.
         *
         * IMMEDIATE, not parked: what a call holds is a `ref_ptr` to an object whose own lifetime the device's
         * tables (or the retirement queue behind them) already decide - dropping the plan's share cannot free
         * anything a submitted frame still names.
         */
        void clear() noexcept
        {
            draws        = nullptr;
            screen       = false;
            draw         = {};
            screen_draw  = {};
            for (::vsg::ref_ptr<::vsg::BindDescriptorSet>& block : blocks) {
                block = {};
            }
            block_count = 0;
            for (::vsg::ref_ptr<::vsg::PushConstants>& push : pushes) {
                push = {};
            }
            push_count = 0;
            for (::vsg::ref_ptr<::vsg::BindVertexBuffers>& bind : vertex_binds) {
                bind = {};
            }
            vertex_bind_count = 0;
        }

        /** @brief Gets the content call's value, with its spans pointed at THIS call's own arrays.
         *
         * WHY THE SPANS ARE NOT STORED: a span into an array that lives in the same value would dangle the moment
         * the value moves (a vector of calls reallocating is exactly that), so a call stores the objects and the
         * counts, and the value handed to the recorder is assembled where it is consumed.
         */
        [[nodiscard]] ContentDraw::Draw value() const noexcept
        {
            ContentDraw::Draw result = draw;
            result.blocks            = std::span<const ::vsg::ref_ptr<::vsg::BindDescriptorSet>>(blocks.data(), block_count);
            result.pushes            = std::span<const ::vsg::ref_ptr<::vsg::PushConstants>>(pushes.data(), push_count);
            result.vertex_binds      = std::span<const ::vsg::ref_ptr<::vsg::BindVertexBuffers>>(vertex_binds.data(), vertex_bind_count);
            return result;
        }
    };

    /** @brief One pass of the frame, resolved: the calls in the order the plan states them. */
    struct Pass {
        core::PassId         pass{ 0 };           // Which pass of the plan this is (see PassContent).
        core::StateRegistry* registry{ nullptr }; // This pass' state memory (the plan owns it, see beginPass).
        std::vector<Call>    calls;               // One per drawing call that resolved and has something to record.

        /** @brief Releases the calls and forgets the pass; the vector keeps its capacity. */
        void clear() noexcept
        {
            pass     = 0;
            registry = nullptr;
            for (Call& call : calls) {
                call.clear();
            }
            calls.clear();
        }
    };

  public:
    ContentPlan() = default;
    ContentPlan(const ContentPlan&)            = delete;
    ContentPlan& operator=(const ContentPlan&) = delete;

  public:
    /** @brief Opens a frame: forgets every pass and the previous frame's leases; the storage is kept.
     *
     * @param reserve_passes How many passes the frame may have (a hint; the storage only ever grows).
     */
    void beginFrame(std::size_t reserve_passes);

    /** @brief Gets the slot for the frame's next pass, with a reset registry of its own.
     *
     * The registry is the pass' state memory (see core::StateRegistry), and it lives HERE rather than on the
     * recording call's stack: the resolution and the recording are two phases of one frame, and the memory that
     * keeps "this variant is bound" true across the second phase's draws has to span both.
     *
     * @param pass The pass identity the content will be placed under (see PassContent).
     * @param pool The device's variant pool (the registry's identity arithmetic).
     * @return The slot to resolve into; valid until the next beginPass or beginFrame.
     */
    [[nodiscard]] Pass& beginPass(core::PassId pass, core::VariantPool& pool);

    /** @brief Gets the passes resolved since beginFrame, in the frame's order. */
    [[nodiscard]] std::span<const Pass> passes() const noexcept;

    /** @brief Pins @p producer for as long as this plan holds the answers it gave (see the file note).
     *
     * @param producer The producer's own lifetime handle (see ContentHalves::pin / ContentSets::pin).
     */
    void pin(Pin producer) noexcept;

    /** @brief Forgets every pass and every lease (a teardown or a frame that will not record). */
    void clear() noexcept;

  public:
    /** @brief Records @p pass' resolved calls into one node - reading no device table. */
    [[nodiscard]] static bool record(const Pass& pass, ::vsg::ref_ptr<::vsg::Node>& out);

  private:
    std::vector<Pass>                                 passes_;     // Slots, reused across frames (see beginPass).
    std::vector<std::unique_ptr<core::StateRegistry>> registries_; // One per slot, reused with the slot.
    std::size_t                                       used_{ 0 };  // Passes resolved since beginFrame.
    Pin                                               halves_;     // The producer whose recorders the calls name.
};

VN_BACKEND_VSG_NS_END
