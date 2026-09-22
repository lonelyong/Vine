#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include <vsg/core/ref_ptr.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ContentHalves.hpp>
#include <vine/vsg/api/ContentImages.hpp>
#include <vine/vsg/api/ContentPass.hpp>
#include <vine/vsg/api/ContentSets.hpp>
#include <vine/vsg/api/ContentStore.hpp>
#include <vine/vsg/api/MaterialImages.hpp>
#include <vine/vsg/api/StreamUploads.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameTimeline.hpp>
#include <vine/vsg/core/RetirementQueue.hpp>
#include <vine/vsg/core/VariantPool.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief One frame's content, assembled and recorded: the tables, the halves, the declared sets and the pass
 * recorder behind TWO calls.
 *
 * WHY IT EXISTS. The pieces each have an owner now - api/ContentStore answers what a plan names out of the
 * live objects, api/ContentHalves compiles the tuples a pass asks for, api/ContentSets builds the declared
 * sets their images imply, api/ContentPass records - and the loop that strings them together (walk the pass,
 * hand each piece the previous one's answer, open the frame's block budget, keep one registry per pass) is
 * the same in every caller. That loop is this class: `beginFrame` and `record`.
 *
 * WHAT THE CALLER STILL OWNS. The device-bound pieces the frame does not decide: the block storage, the
 * variant pool, the material images and the diagnostics (they outlive the assembly and are shared with what
 * else draws), the TARGET's compatibility and its input images (facts of the frame's targets, which the
 * caller resolved), and the view block's bytes (the session owns that convention - see api/ViewBlock). The
 * assembly adds what a pass' recording needs on top: it opens the frame's block budget, and it gives each
 * recorded pass a REGISTRY OF ITS OWN - the registry's own contract is "this pass' state memory", because the
 * state a recording issues is the pass' (its view block's offsets among them) and a second pass must not
 * inherit it.
 *
 * WHAT IT PRODUCES. `record` returns exactly what api/ContentPass::record returns: true when every command
 * was recorded, false when at least one was refused (and reported through the diagnostics the caller gave).
 * A frame recorded before `beginFrame` is refused with an empty node - a caller bug the assembly says
 * nothing about, because the report would be about the caller, not about content.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

/** @brief Assembles and records one frame's content (see the file note). */
class ContentAssembly
{
  public:
    /**
     * @brief Creates the assembly over the pieces that outlive it.
     *
     * @param store       The live content's tables (see api/ContentStore).
     * @param device      The device the produced sets belong to.
     * @param pool        The variant pool the recorders compile against.
     * @param storage     The frame's block storage (the assembly opens its budget in beginFrame()).
     * @param images      The material images the declared sets resolve their maps from.
     * @param diagnostics The backend's one diagnostic route (the recorder reports through it).
     */
    ContentAssembly(ContentStore& store, ::vsg::ref_ptr<::vsg::Device> device, core::VariantPool& pool,
                    BlockStorage& storage, MaterialImages& images, core::Diagnostics& diagnostics);

    /**
     * @brief Opens @p frame: the tables are built for the identities it names, and the block budget opens.
     *
     * @param frame      The compiled plan whose identities the tables must answer.
     * @param timeline   The frame clock parks are dated against (kept for this frame's record calls).
     * @param retirement Where replaced halves and sets are parked (kept for this frame's record calls).
     * @return The tables (the same reference `facts()` answers with).
     */
    [[nodiscard]] const ContentFacts& beginFrame(const core::CompiledFrame& frame, core::FrameTimeline& timeline,
                                                 core::RetirementQueue& retirement);

    /**
     * @brief Assemblies @p pass' halves and declared sets and records it.
     *
     * @param pass          The compiled pass to record.
     * @param compatibility The target's shape (the pipeline key's half - the plan names the target, the
     *                      assembly does not know the caller's targets).
     * @param inputs        The images the pass' inputs offer, one entry per `pass.inputs` entry.
     * @param view_block    The view block's bytes for this pass.
     * @param out           Receives the recorded node (never null).
     * @return true when every command was recorded; false when at least one was refused (reported).
     */
    [[nodiscard]] bool record(const core::CompiledPass& pass, const core::RenderPassCompatibility& compatibility,
                              std::span<const InputImages> inputs, std::span<const std::byte> view_block,
                              ::vsg::ref_ptr<::vsg::Node>& out);

    /** @brief Gets the tables the last beginFrame() answered with (empty before one). */
    [[nodiscard]] const ContentFacts& facts() const noexcept;

    /** @brief Gets the halves the assembly produced for this frame (its counters are evidence). */
    [[nodiscard]] ContentHalves& halves() noexcept;

    /** @brief Gets the declared sets the assembly produced for this frame (its counters are evidence). */
    [[nodiscard]] ContentSets& sets() noexcept;

    ~ContentAssembly();

    ContentAssembly(const ContentAssembly&)            = delete;
    ContentAssembly& operator=(const ContentAssembly&) = delete;

  private:
    struct Data;
    std::unique_ptr<Data> d;
};

V_VSG_NS_END
