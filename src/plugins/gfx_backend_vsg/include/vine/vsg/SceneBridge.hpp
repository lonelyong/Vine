#pragma once
#include "vsg_global.hpp"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include <vsg/commands/Commands.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/Node.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/utils/ShaderSet.h>
#include <vsg/utils/SharedObjects.h>

#include <vine/raw_ptr.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>

namespace vine::graphics
{
class Geometry;
class Scene;
class Node;
class ShaderProgram;
struct RenderCommand;
struct ResolvedRenderState;
}

V_VSG_NS_BEGIN

/**
 * @brief Translates Vine render commands into a retained vsg scene graph.
 *
 * SceneBridge is the bridge between Vine's platform-independent scene graph
 * (Scene/Node/Drawable) and VulkanSceneGraph. Rendering is driven by the
 * per-frame render command list produced by Scene::collectRenderCommands:
 * syncRenderCommands() reconciles a stable vsg::Group against the commands so
 * drawables that move or change color only update their matrix / material in
 * place, and a vsg node (matrix transform -> state group -> vertex draw) is
 * only (re)built when a geometry first appears, disappears, or changes its
 * shape or material binding. This keeps runtime scene edits visible without
 * re-initializing the backend.
 */
class V_VSG_API SceneBridge {
  public:
    SceneBridge();
    ~SceneBridge();

    /** @brief Sets the shader set used to build per-geometry pipelines.
     *
     * @param shaderSet Shader set to use; defaults to flat shaded when unset.
     */
    void setShaderSet(::vsg::ref_ptr<::vsg::ShaderSet> shaderSet);

    /** @brief Sets the material manager used to obtain Phong resources.
     *
     * Must outlive the bridge. When unset, a default VsgMaterialManager is
     * created lazily.
     *
     * @param manager Material manager to use.
     */
    void setMaterialManager(vine::raw_ptr<VsgMaterialManager> manager);

    /** @brief Reconciles the retained scene under root against the commands.
     *
     * Each render command contributes one retained child (a vsg::MatrixTransform
     * holding the geometry state group). Existing children are updated in place;
     * a child is (re)built when its geometry has no cached node or its shape /
     * material binding changed, and children whose geometry is no longer drawn
     * are dropped.
     *
     * @param commands Render commands for the current frame.
     * @param root     Stable vsg root group the retained children live under.
     * @param created  Optional: receives the subtrees newly built this frame
     *                 (they still need GPU compilation before recording).
     * @return true when the graph changed structurally; only newly built
     *         subtrees in @p created require compilation.
     */
    bool syncRenderCommands(
        const std::vector<vine::graphics::RenderCommand>& commands,
        ::vsg::Group* root,
        std::vector<::vsg::ref_ptr<::vsg::Node>>* created = nullptr);

    /** @brief Releases all retained per-geometry vsg nodes. */
    void clearCache();

    /** @brief Sets the pass-level depth policy for content that does not set
     * depth itself.
     *
     * A pass declares how its content treats the target's depth
     * (RenderPass::depthMode): TestAndWrite for opaque scene content, TestOnly
     * for translucent content that tests without writing, Disabled for HUD
     * content drawn on top. The policy fills the depth item of every command
     * that did not author one (RenderCommand::depthExplicit false); a
     * StateNode that sets depth explicitly keeps winning, as it is the
     * finer-grained intent.
     *
     * Changing the policy re-derives the depth state of every command, so call
     * invalidateState() afterwards to rebuild the retained state wrappers
     * (the vertex data is untouched).
     *
     * @param mode Depth handling for this slot's content.
     */
    void setContentDepthMode(vine::graphics::DepthMode mode);

    /** @brief Gets the pass-level depth policy (see setContentDepthMode). */
    vine::graphics::DepthMode contentDepthMode() const { return content_depth_mode_; }

    /** @brief Drops the retained state wrappers so the next sync rebuilds them.
     *
     * The retained subtree is split into vertex DATA (arrays + bind/draw
     * commands) and STATE (pipeline + descriptor binds). Dropping only the
     * state wrappers makes the next syncRenderCommands() rebuild the pipelines
     * for every geometry while reusing the uploaded mesh, which is what a
     * pass-level state change (a new depth policy, a swapped shader set) needs.
     */
    void invalidateState();

    /** @brief Gets the number of distinct compiled pipeline variants.
     *
     * Counts how many genuinely distinct vsg::GraphicsPipeline objects this
     * bridge registered with its shared-objects cache. Geometries that
     * resolve to the same (shader, render state, subpass) variant share one
     * pipeline, so loading N geometries whose variant count stays far below N
     * confirms pipeline sharing is collapsing duplicates (pipeline count
     * follows state variants, not geometry count).
     *
     * @return Number of distinct pipeline variants built so far.
     */
    std::size_t pipelineVariantCount() const { return pipeline_variants_; }

    /** @brief Gets how many times geometry reused a cached pipeline variant.
     *
     * Incremented whenever buildGeometry reuses an already-built (program,
     * material, render-state) template instead of running a fresh
     * GraphicsPipelineConfigurator. A load whose reuse count is close to its
     * geometry count (minus the distinct variants) confirms the L2 fast path
     * is collapsing repeated variant setup.
     *
     * @return Number of variant-template reuses so far.
     */
    std::size_t variantReuseCount() const { return variant_reuses_; }

    /** @brief Gets how many times this bridge ran the glslang stage compiler.
     *
     * The compiler runs once per (program, content revision) — not per vertex
     * layout — so one program used with several custom-channel layouts counts
     * a single compile, proving the L1a stage cache is shared across its
     * layouts (only the per-layout ShaderSet assembly is repeated).
     *
     * @return Number of glslang compile passes started.
     */
    std::size_t programStageCompileCount() const { return program_stage_compiles_; }

  private:
    /** @brief One forwarded custom vertex channel (location >= 3).
     *
     * Describes an extra per-vertex attribute carried past the canonical
     * 0=position / 1=normal / 2=colour arrays: its geometry location and its
     * AttributeBuffer components (the stride its packed floats use). */
    struct VertexChannel
    {
        std::uint32_t location = 0;
        std::uint32_t components = 0;
        bool operator==(const VertexChannel& o) const
        {
            return location == o.location && components == o.components;
        }
    };

    /** @brief Retained per-geometry render node (defined in the .cpp). */
    struct Item;

    /** @brief Builds (or rebuilds) the retained vertex-data node of a geometry.
     *
     * Materialises the geometry's attribute buffers into vsg arrays and wraps
     * them in bind/draw commands. The node is geometry-data only: it carries
     * no pipeline, so it is reused verbatim across material / render-state /
     * program changes (only the state wrapper is rebuilt then), and it stays
     * stable so a later geometry-only edit never re-uploads unchanged meshes.
     * The index stream is kept verbatim (bounds-checked, never truncated):
     * primitive assembly is the topology's job, not the data builder's. When
     * @p opacity_carrier is true (built-in path), the per-vertex colour array
     * is marked DYNAMIC and returned via @p out_colors so per-drawable
     * opacity edits after upload are re-transferred on dirty().
     *
     * @param geometry        Geometry to build.
     * @param opacity_carrier True when the built-in path drives per-drawable
     *                        opacity through the vertex-colour alpha (false
     *                        when a user program owns opacity).
     * @param topology        Primitive topology the geometry is drawn with:
     *                        automatic normal derivation runs for Triangles
     *                        only; Points / Lines fall back to authored
     *                        normals or a constant default.
     * @param out_colors      Receives the per-vertex colour array the caller
     *                        keeps to drive opacity each frame (null when
     *                        @p opacity_carrier is false).
     * @param extra_channels  Receives one entry per forwarded custom channel
     *                        (location >= 3), in binding order after the three
     *                        canonical arrays (ascending location).
     * @return Data commands node, or null when not buildable.
     */
    ::vsg::ref_ptr<::vsg::Commands> buildGeometryData(
        vine::raw_ptr<const vine::graphics::Geometry> geometry,
        bool opacity_carrier,
        vine::graphics::Topology topology,
        ::vsg::ref_ptr<::vsg::vec4Array>& out_colors,
        std::vector<VertexChannel>& extra_channels);

    /** @brief Builds (or rebuilds) the state wrapper around a data node.
     *
     * The wrapper is a vsg::StateGroup carrying the pipeline + descriptor-set
     * binds for one (program, material, resolved-state, vertex-layout) variant;
     * @p data is attached as its child by the caller. Pipelines are resolved
     * through the per-(program, layout) ShaderSet cache and the per-variant L2
     * template cache, so repeated variants skip the configurator entirely. The
     * forwarded custom channels (locations >= 3) are bound after the canonical
     * three arrays and named vine_Attribute{location}.
     *
     * @param data           The retained vertex-data node to wrap (non-null).
     * @param material       Bound material (may be null).
     * @param state          Resolved render state the pipeline must honour.
     * @param program        User shader program, or null for the built-in
     *                       default.
     * @param extra_channels Custom channels carried by @p data (locations >= 3),
     *                       in binding order after the three canonical arrays.
     * @return State wrapper, or null when not buildable.
     */
    ::vsg::ref_ptr<::vsg::StateGroup> buildStateGroup(
        ::vsg::ref_ptr<::vsg::Node> data,
        vine::raw_ptr<vine::graphics::Material> material,
        const vine::graphics::ResolvedRenderState& state,
        vine::raw_ptr<const vine::graphics::ShaderProgram> program,
        const std::vector<VertexChannel>& extra_channels);

    /** @brief Gets (and caches) the run-time compiled ShaderSet for a program.
     *
     * Compiles the program's stages and assembles a ShaderSet once per
     * (program, vertex layout) instead of once per geometry: N geometry bound
     * to the same program AND carrying the same set of custom channels share a
     * single glslang compile and ShaderSet. Besides the canonical
     * vsg_Vertex/Normal/Color bindings (locations 0/1/2), the set declares one
     * vine_Attribute{location} binding per forwarded custom channel, whose
     * format follows its components. A compile/assembly failure is cached too
     * (null), so later geometry does not retry the failed compile each time.
     *
     * @param program        User program (non-null).
     * @param extra_channels Custom channels (locations >= 3) the set must
     *                       declare, in binding order.
     * @return Compiled shader set, or null when it could not be built.
     */
    ::vsg::ref_ptr<::vsg::ShaderSet> getProgramShaderSet(
        vine::raw_ptr<const vine::graphics::ShaderProgram> program,
        const std::vector<VertexChannel>& extra_channels);

    /** @brief Gets the material manager used to obtain Phong resources.
     *
     * Falls back to the bridge-owned default manager when the renderer never
     * injected one (setMaterialManager). Both callers that need material
     * resources route through here so the fallback is decided once.
     *
     * @return The active material manager (always non-null).
     */
    VsgMaterialManager& materialManager();

    /** @brief Gets the slot's base shader set (the built-in default when unset).
     *
     * The built-in Phong set is created lazily on first use and cached, so
     * the bridge never pays for a fresh createPhongShaderSet() per geometry.
     * A user program path builds on top of this set's default pipeline states
     * (the baked viewport / blending), keeping both paths on one material
     * descriptor ABI.
     *
     * @return The base shader set (always non-null).
     */
    ::vsg::ref_ptr<::vsg::ShaderSet> baseShaderSet();

    ::vsg::ref_ptr<::vsg::ShaderSet> shader_set_;
    // Pass-level depth policy applied to commands that did not author depth
    // (see setContentDepthMode); part of the retained state identity, so
    // changing it invalidates the state wrappers.
    vine::graphics::DepthMode content_depth_mode_ = vine::graphics::DepthMode::TestAndWrite;
    // Shares layout / pipeline / descriptor-set content across every geometry
    // this bridge builds: GraphicsPipelineConfigurator::copyTo() deduplicates
    // through SharedObjects (content equality), so geometry that resolves to
    // the same pipeline state and material registers ONE vsg::GraphicsPipeline
    // and descriptor set instead of one per geometry.
    ::vsg::ref_ptr<::vsg::SharedObjects> shared_objects_;
    // Distinct pipeline variants registered with shared_objects_ (diagnostic;
    // see pipelineVariantCount()).
    std::size_t pipeline_variants_ = 0;
    // Times buildGeometry reused a cached (program, material, state) template
    // instead of running a fresh configurator (diagnostic; see
    // variantReuseCount()).
    std::size_t variant_reuses_ = 0;
    // Fresh glslang stage compiles started (L1a; diagnostic — see
    // programStageCompileCount()).
    std::size_t program_stage_compiles_ = 0;
    vine::raw_ptr<VsgMaterialManager> material_manager_ = nullptr;
    // Default manager used when the renderer does not inject one.
    VsgMaterialManager default_manager_;
    // Retained per-geometry nodes, keyed by geometry pointer for O(1) lookup.
    std::unordered_map<const vine::graphics::Geometry*, std::unique_ptr<Item>> cache_;
    // Geometries whose data was rejected (malformed attributes / out-of-range
    // indices), keyed by the data revision they were rejected at. Kept so the
    // rejection diagnostic prints once per revision instead of on every frame;
    // a data revision change (or the geometry leaving the frame) clears it.
    std::unordered_map<const vine::graphics::Geometry*, std::uint64_t> rejected_;
    // Cached run-time compiled ShaderSet per (user program, vertex layout) (L1):
    // every geometry bound to the same program with the SAME set of forwarded
    // custom channels shares one glslang compile + ShaderSet instead of
    // recompiling per geometry; a different custom-channel layout (or program)
    // is its own entry. Keyed by a content hash; the entry stores the full
    // (program, layout, program revision) key for collision-safe equality and
    // so editing a retained program's GLSL (ShaderProgram::revision) rebuilds
    // the compiled set instead of serving the stale one (D10).
    struct ProgramEntry
    {
        const vine::graphics::ShaderProgram* program = nullptr;
        std::uint64_t layout = 0;      // hash over the custom channels
        std::uint64_t revision = ~std::uint64_t{0};
        ::vsg::ref_ptr<::vsg::ShaderSet> shader_set;
    };
    std::unordered_map<std::uint64_t, std::unique_ptr<ProgramEntry>>
        program_shader_sets_;
    // Compiled SPIR-V stages per (user program, content revision) (L1a): the
    // glslang pass runs ONCE per program content; every vertex layout of that
    // program then shares these stages and only the ShaderSet assembly differs
    // (L1b, program_shader_sets_). Editing a program bumps its revision and
    // forces a fresh compile (D10).
    struct StageEntry
    {
        std::uint64_t revision = ~std::uint64_t{0};
        ::vsg::ShaderStages stages;
    };
    std::unordered_map<const vine::graphics::ShaderProgram*, StageEntry>
        program_stages_;
    // Pipeline-template cache (L2), keyed by a content hash of the (program,
    // material, resolved-state) variant; the full key lives in VariantEntry
    // for collision-safe equality. The first geometry of a variant builds its
    // pipeline through the configurator and captures the reusable bind
    // commands; later geometry of that variant reuse them and only attach
    // their own vertex data, keeping pipeline setup cost proportional to the
    // state-variant count rather than the geometry count.
    struct VariantEntry;
    std::unordered_map<std::uint64_t, std::unique_ptr<VariantEntry>> variant_cache_;
};

V_VSG_NS_END
