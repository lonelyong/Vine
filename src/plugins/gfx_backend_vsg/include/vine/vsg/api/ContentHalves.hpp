#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ContentPass.hpp>
#include <vine/vsg/api/StateCommands.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameTimeline.hpp>
#include <vine/vsg/core/RetirementQueue.hpp>
#include <vine/vsg/core/VariantPool.hpp>
#include <vine/vsg/vsg_global.hpp>

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
 * revision, the vertex layout, the variant, and the pass' COLOUR ATTACHMENT COUNT (the layer's blend state
 * declares it). Two passes that agree on all of it share the half; one that differs anywhere gets its own.
 * That last field is not a detail: a layer created for one attachment compiles blend states for one, and a
 * pass with four would draw into three of them, silently.
 *
 * WHERE THE FACTS COME FROM. The CALLER's tables (api/ContentStore produces them from the live objects).
 * The producer makes no lookup of its own beyond the ones the RECORDER will make: the geometry the plan
 * names at the revision the plan names, the material by identity, the variant both imply, and the program
 * entry for that variant. So a half exists for exactly the tuples a pass asks for - and when the tables
 * cannot answer one, no half is invented for it (the pass reports the miss; it is the same miss either way).
 *
 * WHAT IT DOES NOT DO, deliberately. It does not build the pass' DECLARED SETS: those carry the caller's
 * images and the frame's descriptor numbers, which is the scope's business (api/BlockDescriptors). It also
 * does not grow: a half whose key the tables STOP answering (its revision was retired) is parked, the same
 * retirement window every other replaced object gets - so what is compiled tracks what the tables answer.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

/** @brief Produces the compiled halves a pass' draws need (see the file note). */
class ContentHalves
{
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
     * A half whose key the tables no longer answer (the tables retire a superseded revision) is parked
     * through @p retirement, dated against @p timeline.
     *
     * @param pass       The compiled pass the halves are for (its attachment count is part of their key).
     * @param facts      The tables the identities are answered from.
     * @param timeline   The frame clock parks are dated against.
     * @param retirement Where halves whose key left the tables are parked.
     * @return The entries, valid until the next call - they borrow this producer's own storage.
     */
    [[nodiscard]] std::span<const ContentPass::Scope::Entry> halvesFor(const core::CompiledPass& pass,
                                                                       const ContentFacts& facts,
                                                                       core::FrameTimeline& timeline,
                                                                       core::RetirementQueue& retirement);

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

V_VSG_NS_END
