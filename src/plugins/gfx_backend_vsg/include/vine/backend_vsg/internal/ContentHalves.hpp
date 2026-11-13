#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <vine/graphics/backend/FrameCompiler.hpp>
#include <vine/graphics/backend/FrameTimeline.hpp>
#include <vine/graphics/backend/RetirementQueue.hpp>
#include <vine/graphics/backend/VariantPool.hpp>
#include <vine/backend_vsg/internal/ContentFacts.hpp>
#include <vine/backend_vsg/internal/ContentPass.hpp>
#include <vine/backend_vsg/internal/StateCommands.hpp>
#include <vine/backend_vsg/backend_vsg_global.hpp>

/**
 * @brief The compiled HALVES a pass needs, produced from the tables (see api/ContentPass for what a half is).
 *
 * WHY IT EXISTS. A scope's entries are one per (program, revision, layout, VARIANT), and every one of them is
 * backed by a compiled pipeline layer and its recorder. Until this class a host built those itself, per pass,
 * from the facts: a loop that found each command's variant, looked its program entry up by it, described the
 * geometry's channels as a vertex layout and created a layer. Every host would write that loop, and the
 * places it can go subtly wrong are exactly the places the identity is spelled - the variant, the layout's
 * binding numbers, the pass' attachment count - so it lives here, once.
 *
 * WHAT A HALF IS KEYED BY. Everything the compiled object depends on: the drawing KIND, the program, its
 * revision, the vertex layout, the variant, the pass' COLOUR ATTACHMENT COUNT (the layer's blend state
 * declares it) and the TOPOLOGY the pipeline bakes (the API restricts a dynamically set topology to the
 * class its pipeline was created with - see core::PipelineKey::topology). Two passes that agree on all of it
 * share the half; one that differs anywhere gets its own. The attachment count is not a detail: a layer
 * created for one attachment compiles blend states for one, and a pass with four would draw into three of
 * them, silently.
 *
 * WHERE THE FACTS COME FROM. The CALLER's tables (api/ContentStore produces them from the live objects).
 * The producer makes no lookup of its own beyond the ones the RECORDER will make: the geometry the plan
 * names at the revision the plan names, the material by identity, the variant both imply, and the program
 * entry for that variant. So a half exists for exactly the tuples a pass asks for - and when the tables
 * cannot answer one, no half is invented for it (the pass reports the miss; it is the same miss either way).
 *
 * WHAT IT DOES NOT DO, deliberately. It does not build the pass' DECLARED SETS: those carry the caller's
 * images and the frame's descriptor numbers, which is the scope's business (api/BlockDescriptors). Its lifetime
 * is TWO PINS: the program revision it was compiled from (`findProgram`), and the LAYOUT declaration index
 * (`ContentDeclarations::declaresLayout` - how many live geometry rows still feed that vertex layout). A layout
 * whose last geometry left the tree therefore retires its halves. In production both questions are O(1); a
 * hand-built table that published no layout index answers the second by scanning its rows (the optional-index
 * rule `ContentFacts` states for the row orders too). The departure is parked through the same window every
 * other replaced object gets.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
VN_BACKEND_VSG_NS_BEGIN

/** @brief Produces the compiled halves a pass' draws need (see the file note). */
class ContentHalves {
  public:
    /**
     * @brief Creates the producer.
     *
     * @param pool         The variant pool the recorders compile against (the scope's).
     * @param entry_points The device's dynamic-state entry points; a device-free caller leaves them empty
     *                     (see api/ContentDraw - the halves can then be produced and looked at, but not
     *                     recorded).
     */
    explicit ContentHalves(core::VariantPool& pool, detail::DynamicStateEntryPoints entry_points = {});

    /**
     * @brief Ensures the halves @p pass' draws need, from @p facts, and returns them.
     *
     * The entries are in first-seen order, one per (program, revision, layout, variant) the walk names - the
     * order the plan draws in, deduplicated. A drawable the tables cannot answer produces no entry: the
     * recorder refuses it on the geometry or the material first, and a half invented for one would be a half
     * nobody could find.
     *
     * WHAT IT DOES NOT DO. It does not decide what may go: retirement is the FRAME's step, once per frame, even
     * for a frame that draws nothing (see @ref sweep). Asking each pass would make "when does a half die"
     * depend on whether some pass happened to walk this producer.
     *
     * @param pass       The compiled pass the halves are for (its attachment count is part of their key).
     * @param facts      The tables the identities are answered from.
     * @param timeline   The frame clock (unused here; kept for the caller's symmetry with the other producers).
     * @param retirement Unused here (the parks happen in @ref sweep); kept for the same reason.
     * @return The entries, valid until the next call - they borrow this producer's own storage.
     */
    [[nodiscard]] std::span<const ContentPass::Scope::Entry>
    halvesFor(const core::CompiledPass& pass, const ContentFacts& facts, core::FrameTimeline& timeline, core::RetirementQueue& retirement);

    /**
     * @brief Parks every half the tables no longer answer, once per frame.
     *
     * @details
     * - THE FRAME'S STEP, not a pass': a half is pinned by its program revision and its layout declaration (see the
     *   file note), both facts of the tables rather than of any pass - so a frame that draws nothing still retires
     *   the halves whose content left, exactly as it retires the streams and the arena slots.
     * - Called from the content frame's opening (see ContentAssembly::beginFrame / prepare), before any pass asks
     *   for its halves: a half this frame will need is still pinned, so the order costs nothing.
     * - A refused build has no GPU object to park; its key is simply forgotten once the same pins are gone.
     *
     * @param facts      The tables the identities are answered from.
     * @param timeline   The frame clock parks are dated against.
     * @param retirement Where the departed halves are parked.
     */
    void sweep(const ContentFacts& facts, core::FrameTimeline& timeline, core::RetirementQueue& retirement);

    /** @brief Gets how many compiled halves are alive (a steady pass raises this by nothing). */
    [[nodiscard]] std::size_t halves() const noexcept;

    /** @brief Gets how many layers were built in total. A REFUSED build counts once: it is never retried. */
    [[nodiscard]] std::uint64_t builds() const noexcept;

    /** @brief Gets how many keys were refused (a program whose GLSL did not compile, a channel with no bind). */
    [[nodiscard]] std::size_t refused() const noexcept;

    /** @brief Releases every half. The session-teardown path: nothing is parked. */
    void clear();

    ~ContentHalves();

    ContentHalves(const ContentHalves&)            = delete;
    ContentHalves& operator=(const ContentHalves&) = delete;

  private:
    struct Data;
    // A shared Data rather than a unique one: a parked removal holds a WEAK reference to it, so a producer
    // that dies before its parks come due releases nothing and dangles nowhere.
    std::shared_ptr<Data> d;
};

VN_BACKEND_VSG_NS_END
