#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Group.h>
#include <vsg/state/BindDescriptorSet.h>
#include <vsg/state/ImageView.h>

#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ProgramVariant.hpp>
#include <vine/vsg/api/ContentImages.hpp>
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
 * THE PASS' SAMPLED INPUTS. The plan says what a pass reads (identity plus how many colour textures each input
 * offers); the caller says what the IMAGES are, one entry per plan entry, and the two have to agree before
 * anything is bound: a pass whose world does not match its plan would sample a picture nobody described. When
 * they agree, the colour textures are bound in the layer's sampled-input set (set 1, one binding per texture in
 * declaration order) and the count travels in the pipeline's identity - so a pipeline is compiled against
 * exactly the sampled shape its pass binds, and a second draw of the same pass binds the set once.
 *
 * WHAT THE OUTER LAYER OWNS, and is therefore not here: turning an input identity into images. The targets
 * belong to the session (the same table the executor resolves its targets from), so the caller - the layer that
 * owns them - offers the images by identity; this layer binds what it was offered and refuses when it does not
 * match the plan. Both halves of the input ABI are bound (colour attachments and a sampleable depth), and
 * WHICH one an input offers is the plan's fact - the caller is checked against it rather than asked.
 *
 * WHAT IS NOT HERE YET, and is therefore not promised: the full-screen push block's CONTENTS (the lights and the
 * depth reconstruction the SDK's deferred lighting program reads) arrive with the full-screen lighting half - the
 * forward path's light and shadow blocks are bound here (see LightBlock / ShadowBlock), and the full-screen push is
 * recorded empty rather than approximated.
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
 * THE OTHER DRAWING CALL: FULL-SCREEN PASSES. A `DrawKind::Screen` drawing call draws the same pass' declared
 * inputs through a program of its own - no geometry, no vertex streams, no material - so its half is an entry
 * of its own KIND (see `Scope::Entry::kind`): a full-screen pipeline layer, looked up by the plan's program
 * identity and revision. Nothing about the lookup is geometry-shaped, which is why a screen entry carries no
 * vertex layout. The pass' images reach it through the SAME check and the same offer as the content halves,
 * but the set they are bound in is the FULL-SCREEN ABI's (see ContentPipeline): the call's source (named by
 * idENTITY the plan resolved) offers bindings 0.. for its attachments, the shadow map takes the binding its
 * text declares, and the call's own shadow block takes its own - the pass builds that set per call, because
 * a full-screen call has ONE block and it is the call's. The state it draws with is the plan's
 * `CompiledDraw::dynamic` (a full-screen call has no per-command state to resolve), and its 128-byte push
 * block carries the call's lights (see `recordScreenDraw`) - the full-screen ABI's own copy of what the
 * content path's light block is.
 */
V_VSG_NS_BEGIN

/** @brief The content recorder of one pass scope (see the file note for what it refuses and why). */
class V_VSG_API ContentPass
{
  public:
    /** @brief The pieces this layer drives; the caller owns them and keeps them alive. */
    struct Scope
    {
        /** @brief One compiled half: one program's stages against one vertex layout, for ONE variant. */
        struct Entry
        {
            core::DrawKind        kind{core::DrawKind::Content};  ///< Which drawing call this half serves.
            const void*           program{nullptr};    ///< The program identity the plan names.
            std::uint64_t         revision{0};         ///< The revision the stages were taken at.
            core::VertexLayoutKey layout{};            ///< The vertex layout its pipeline declares (content).
            ContentPipeline*      pipelines{nullptr};  ///< The pipeline layer over that stage text.
            ContentDraw*          draws{nullptr};      ///< The recorder over those pipelines.
            /// The VARIANT this half was compiled for (see api/ProgramVariant). One program has several,
            /// and they share everything above - the same identity, revision and layout - so without this
            /// field a pass would hand every drawable of that program the FIRST half it finds and compile
            /// one of the variants' meanings into all of them. A pass computes each command's variant from
            /// its material and geometry (api/ProgramVariant's rule) and only a half that serves it draws.
            /// It sits LAST so an entry written before this field existed is the variant the untagged
            /// facts describe (the empty define list), not one with a pointer in its switches.
            ProgramVariant        variant{};
        };

        std::span<const Entry> entries;               ///< One per (program, revision, layout, VARIANT) it draws with.
        core::StateRegistry*   registry{nullptr};     ///< This pass' state memory, shared by every half.
        BlockStorage*          storage{nullptr};      ///< The frame's block storage.
        /// The block sets this pass may bind: ONE PER SET INDEX A PROGRAM DECLARES BLOCKS IN, built from the
        /// shape that program declares (`BlockDescriptors::create(device, storage, layer->blockShape(set),
        /// set)`). A program whose declared shape no entry here covers is refused by name - the alternative
        /// is binding a set whose layout the pipeline was not compiled against.
        std::span<BlockDescriptors* const> block_sets{};
        StreamUploads*         uploads{nullptr};      ///< The stream sharing (geometry).
        /// Episode state of the light-drop report: a drawing call whose announced lights all fit the block
        /// re-arms it, so "the host announced lights the block cannot carry" is said once per episode rather
        /// than once per drawing call (see `record`). The episode's END is the caller's decision - a scope
        /// that lives for one frame reports once per frame, and one that lives for the session reports once.
        core::ReportOnce       lights_dropped;
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
     * @param pass          The compiled pass (its draws, viewports, dynamic state and declared inputs).
     * @param facts         The tables the identities are answered from.
     * @param compatibility The target's shape (the pipeline key's half - the plan names the target, this layer
     *                      does not know it).
     * @param inputs        The images the pass' inputs offer, one entry per `pass.inputs` entry and in the
     *                      same order; a mismatch (a different number of entries, or a different number of
     *                      colour textures on one of them) is reported and refuses the whole pass.
     * @param view_block    The view block's bytes for this pass (the session owns that convention).
     * @param out           Receives the recorded node, or a node with nothing in it when every command was
     *                      refused (never null).
     * @return true when every command was recorded; false when at least one was refused (it was reported).
     */
    bool record(const core::CompiledPass& pass, const ContentFacts& facts,
                const core::RenderPassCompatibility& compatibility, std::span<const InputImages> inputs,
                std::span<const std::byte> view_block, ::vsg::ref_ptr<::vsg::Node>& out);


  private:
    /** @brief The most channels one geometry may feed through this layer (beyond it the command is refused). */
    static constexpr std::size_t kMaxChannels = 8;

    /** @brief Records one command; false when it was refused (and reported). */
    bool recordCommand(const core::CompiledCommand& command, const core::CompiledDraw& draw,
                       const core::CompiledPass& pass, const ContentFacts& facts,
                       const core::RenderPassCompatibility& compatibility, std::uint64_t view_offset,
                       std::uint64_t lights_offset, std::uint64_t shadow_offset,
                       const ::vsg::ref_ptr<::vsg::BindDescriptorSet>& inputs,
                       std::uint32_t sampled_color_count, std::uint32_t sampled_depth_count,
                       ::vsg::Group& into);

    /** @brief Reports the lights of @p announced that the block could not carry, once per episode. */
    void reportLightsDropped(std::size_t announced, std::size_t represented, bool has_camera);

    /** @brief Reports, once per half, a program that cannot read the shadow its pass declared.
     *
     * The pass' plan resolves a map only from a target that STATES one (see core::ShadowFacts), so "this
     * pass samples a shadow" is a fact of the plan; whether the program's text can read it is a fact of its
     * declarations, and the name is what says so (api/ContentImages's `samplesShadowMap`). The two together
     * are a diagnostic nothing else in the frame can give: the drawables stay lit and the map is simply never
     * sampled, so without this line the shadow "disappears" with no reason anywhere.
     *
     * @param entry The compiled half that is about to draw.
     * @param pass  The compiled pass (its resolved shadow).
     */
    void reportShadowNotSampled(const Scope::Entry& entry, const core::CompiledPass& pass);

    /** @brief Builds the pass' sampled-input set and its bind command for a CONTENT half, or null when there
     *         is nothing to bind.
     *
     * One set per pass: the inputs are a property of the pass, so every draw of the content kind binds the
     * same set, and the registry's "already bound" answer (see StateRegistry) is what keeps the second draw
     * from re-issuing it. The set lives at `ContentPipeline::kInputSet` (1, after the declared block sets),
     * which is where a content layer's sampled inputs live - a full-screen call's set is built by
     * `recordScreenDraw` instead, in the ABI its own text declares.
     *
     * @param pass   The compiled pass whose inputs are being bound.
     * @param inputs The images the caller offered, already checked against the plan.
     * @param layer  The half's pipeline layer (its layout is what the set is built against).
     * @return The bind command, or null for a pass with no colour textures to sample (a build failure is
     *         reported here).
     */
    ::vsg::ref_ptr<::vsg::BindDescriptorSet> makeInputSet(const core::CompiledPass& pass,
                                                         std::span<const InputImages> inputs,
                                                         ContentPipeline& layer);

    /** @brief Records one FULL-SCREEN drawing call; false when it was refused (and reported).
     *
     * A full-screen call draws ONE of the pass' inputs - its source, named by the idENTITY the plan resolved -
     * plus the shadow map (by name, wherever its text declares it), through the half its program names, with
     * the plan's `CompiledDraw::dynamic` - a full-screen call has no per-command state to resolve.
     *
     * IT BUILDS THE SET ITSELF, and that is the difference from a content half: the source's attachments, the
     * map and the call's block are the PASS' images, and a full-screen text declares where they live (see
     * ContentPipeline). The block is the call's own (its lights and camera are), so the set is built per call;
     * the engine's passes make one full-screen call each.
     *
     * WHAT IT REFUSES, and each by name: a source that is not one of the pass' declared inputs; an input that
     * is neither the source nor the map; a text that declares `shadow_map` when the pass resolved no readable
     * map (a text that names the map is a text that shades a shadow - the engine picks its shadowed variant
     * only when a shadow exists); and a text that samples a texture this pass does not offer.
     *
     * THE PUSH BLOCK IS THE FULL-SCREEN ABI'S 128 BYTES AND CARRIES THE CALL'S LIGHTS
     * (`LightPushBlock` - the same packing the lighting phase uses for the content path's block).
     *
     * @param draw          The compiled full-screen call.
     * @param entry         The screen half its program names (resolved by the caller).
     * @param pass          The compiled pass (its colour attachment count, its inputs and its resolved shadow).
     * @param compatibility The target's shape (the pipeline key's half - the plan names the target, this layer
     *                      does not know it).
     * @param inputs        The images the pass' inputs offer, one entry per `pass.inputs` entry and in the same
     *                      order (the same offer the content halves get).
     * @param into          The group the recorded commands are added to.
     * @return true when recorded; false when the half could not record it (already reported).
     */
    bool recordScreenDraw(const core::CompiledDraw& draw, const Scope::Entry& entry,
                          const core::CompiledPass& pass, const core::RenderPassCompatibility& compatibility,
                          std::span<const InputImages> inputs, ::vsg::Group& into);

    /** @brief Reports one refused command, naming the identity and the reason the lookup gave. */
    void reportRefused(const char* what, FactMiss miss);

    /** @brief Reports a refused command for a reason that is not a table miss. */
    void reportRefused(const char* what, const char* why);


  private:
    /** @brief Resolves the block sets @p entry's program declares, in the order the declarations name them.
     *
     * This is the pass' ONE place that knows what it can fill today: a program's declared sets must be
     * covered by the sets the caller built for it (same index, same shape). A half that fails the check is
     * reported once per pass and its commands are not recorded - the rest of the pass still draws. The push
     * ranges a program declares are NOT checked here: they are filled per command (see api/ContentPush), and
     * a range nobody can fill is refused where the layout is built (ContentPipeline::create).
     *
     * @param entry       The compiled half.
     * @param input_count Sampled textures the pass' key carries (colours plus depths).
     * @return true when the half can be served; @ref half_blocks_ then holds the sets to bind.
     */
    [[nodiscard]] bool serveHalf(const Scope::Entry& entry, std::uint32_t input_count);

  private:
    /** @brief The most block sets one program can declare and still be served (the five roles' bound). */
    static constexpr std::size_t kMaxBlockSets = 8U;

    /** @brief The most push ranges one program can declare and still be served (the engine declares one). */
    static constexpr std::size_t kMaxPushRanges = 4U;

    Scope               scope_;        ///< The pieces this layer drives (borrowed).
    core::Diagnostics&  diagnostics_;  ///< The one diagnostic route.
    /// One report-once per entry: a half that cannot be served must say so once, not once per command.
    std::vector<core::ReportOnce> half_reported_;
    /// One report-once per entry for the shadow the pass declared and the program cannot read (see
    /// `reportShadowNotSampled`): a second message about the same half must not be what silences the first.
    std::vector<core::ReportOnce> shadow_reported_;
    /// The block sets `serveHalf` resolved for the half the command being recorded draws
    /// through (in the declared set order).
    std::array<BlockDescriptors*, kMaxBlockSets> half_blocks_{};
    std::size_t                                  half_block_count_{0};
    /// The push commands one command's declared ranges recorded (reused per command: the record is consumed
    /// by the recorder within the call, so no per-draw container is allocated).
    std::array<::vsg::ref_ptr<::vsg::PushConstants>, kMaxPushRanges> push_commands_{};
    /// The push bytes being assembled, reused across commands (a steady frame does not allocate for them).
    std::vector<std::byte> push_bytes_{};
    /// The member name a refused push range carried (only read while reporting).
    std::string_view unhandled_member_{};
};

V_VSG_NS_END
