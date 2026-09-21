#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Group.h>

#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/StreamUploads.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief Recording a compiled pass' CONTENT, from the three tables to the graph the executor will place.
 *
 * WHERE THIS SITS. The plan (CompiledPass) says what the pass means: which drawing calls, which viewport, which
 * dynamic state. The tables (ContentFacts) say what the identities in it ARE: this geometry's channels, this
 * program's stages, this material's bytes. This layer is the one that puts the two together - and it is the last
 * place a mistake can still be caught cheaply, because everything above it is a plan and everything below it is
 * GPU work.
 *
 * WHAT IT REFUSES, ONE COMMAND AT A TIME. A command whose program, geometry or material the tables cannot
 * answer is NOT drawn and is reported with the reason the lookup gave (unknown / a different revision /
 * malformed). The rest of the pass still draws: one unshaded drawable is a hole in the picture, not a reason to
 * lose the frame - the same "draw what you can, say what you could not" rule the engine's own content path
 * follows.
 *
 * WHAT THE SCOPE IS A SET OF: COMPILED HALVES. A pipeline layer is built from ONE program's stage text against
 * ONE vertex layout, so the scope holds a set of them - one per (program, revision, layout) the pass draws with
 * - and picks by the pair the command and its geometry name. What the halves SHARE is the pool and the pass'
 * registry: sharing the pool is what keeps one identity one variant, and sharing the registry is what keeps
 * "what is bound right now" true across a pass that alternates halves - a registry per half would skip the
 * pipeline bind the other half had already replaced, and the draw would run with a pipeline that is not its
 * own. A command whose pair no half was built for is refused, and the message says WHICH of the two did not
 * match, because the two have different fixes (compile the program / compile the layout).
 *
 * WHAT IS NOT HERE YET, and is therefore not promised: the pass' SAMPLED INPUTS (the plan does not carry them
 * yet, so a key says zero sampled attachments) and full-screen drawing calls (they arrive with the program-slot
 * path). Both are refused rather than approximated.
 */
V_VSG_NS_BEGIN

/**
 * @brief The content recorder of one pass scope (see the file note for what it refuses and why).
 */
class V_VSG_API ContentPass
{
  public:
    /** @brief The pieces this layer drives; the caller owns them and keeps them alive. */
    struct Scope
    {
        /** @brief One compiled half: one program's stages against one vertex layout. */
        struct Entry
        {
            const void*           program{nullptr};    ///< The program identity the plan names.
            std::uint64_t         revision{0};         ///< The revision the stages were taken at.
            core::VertexLayoutKey layout{};            ///< The vertex layout its pipeline declares.
            ContentPipeline*      pipelines{nullptr};  ///< The pipeline layer over that stage text.
            ContentDraw*          draws{nullptr};      ///< The recorder over those pipelines.
        };

        std::span<const Entry> entries;               ///< One per (program, revision, layout) this pass draws with.
        core::StateRegistry*   registry{nullptr};     ///< This pass' state memory, shared by every half.
        BlockStorage*          storage{nullptr};      ///< The frame's block storage.
        BlockDescriptors*      descriptors{nullptr};  ///< The block set (one per frame).
        StreamUploads*         uploads{nullptr};      ///< The stream sharing (geometry).
    };

  public:
    /** @brief Creates the recorder for one pass scope.
     *
     * @param scope       The pieces to drive; every shared pointer must be non-null, and every entry must name
     *                    a program, a layout, a pipeline layer and a recorder.
     * @param diagnostics The backend's one diagnostic route.
     */
    ContentPass(const Scope& scope, core::Diagnostics& diagnostics) noexcept;

    ContentPass(const ContentPass&)            = delete;
    ContentPass& operator=(const ContentPass&) = delete;

    /** @brief Records one compiled pass' content.
     *
     * The caller has already opened the frame's block budget (BlockStorage::beginFrame) and owns the
     * descriptors, streams and pipelines this scope drives.
     *
     * @param pass          The compiled pass (its draws, viewports and dynamic state).
     * @param facts         The tables the identities are answered from.
     * @param compatibility The target's shape (the pipeline key's half - the plan names the target, this layer
     *                      does not know it).
     * @param view_block    The view block's bytes for this pass (the session owns that convention).
     * @param out           Receives the recorded node, or a node with nothing in it when every command was
     *                      refused (never null).
     * @return true when every command was recorded; false when at least one was refused (it was reported).
     */
    bool record(const core::CompiledPass& pass, const ContentFacts& facts,
                const core::RenderPassCompatibility& compatibility, std::span<const std::byte> view_block,
                ::vsg::ref_ptr<::vsg::Node>& out);


  private:
    /** @brief The most channels one geometry may feed through this layer (beyond it the command is refused). */
    static constexpr std::size_t kMaxChannels = 8;

    /** @brief Records one command; false when it was refused (and reported). */
    bool recordCommand(const core::CompiledCommand& command, const core::CompiledDraw& draw,
                       const core::CompiledPass& pass, const ContentFacts& facts,
                       const core::RenderPassCompatibility& compatibility, std::uint64_t view_offset,
                       ::vsg::Group& into);

    /** @brief Reports one refused command, naming the identity and the reason the lookup gave. */
    void reportRefused(const char* what, FactMiss miss);

    /** @brief Reports a refused command for a reason that is not a table miss. */
    void reportRefused(const char* what, const char* why);


  private:
    Scope               scope_;        ///< The pieces this layer drives (borrowed).
    core::Diagnostics&  diagnostics_;  ///< The one diagnostic route.
};

V_VSG_NS_END
