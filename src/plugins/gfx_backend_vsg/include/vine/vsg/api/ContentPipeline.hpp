#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/DescriptorSetLayout.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/state/PipelineLayout.h>
#include <vsg/state/Sampler.h>
#include <vsg/state/ShaderStage.h>

#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/ProgramAbi.hpp>
#include <vine/vsg/core/Keys.hpp>
#include <vine/vsg/core/VariantPool.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The compiled half of content drawing: one `vsg::GraphicsPipeline` per identity, with the DYNAMIC
 * half of the state declared so a host's state change can never need a new one.
 *
 * WHAT THE PIPELINE IS ALLOWED TO CARRY. Depth comparison, culling, front face, polygon mode, topology and
 * blend are declared DYNAMIC here, and so are the viewport and the scissor: those are exactly the values a
 * `StateNode` edits while a scene runs, and a pipeline that baked them would need one compile per
 * combination - the failure family this backend has shipped (`StateNode` edit => recompile) and the one it
 * nearly shipped (a resize => every full-screen slot rebuilt, because an extent had reached a key). The
 * values the create-info still carries are BAKED CONSTANTS: Vulkan requires the structs, and the effective
 * values come from the set commands a draw records.
 *
 * WHAT DECIDES IDENTITY. `core::VariantPool` does, from `core::PipelineKey` - the program and its revision,
 * the vertex layout, the render-pass compatibility, and whether a depth texture or a shadow map is bound.
 * This class only builds the object the pool says is needed and keeps it alive; the pool can evict an id,
 * and this layer then drops its object too (the commands that bind a pipeline hold their own reference, so
 * what dies here is only a keep-alive).
 *
 * WHERE THE BLOCKS LIVE IS THE PROGRAM'S ANSWER. A descriptor set layout is not this backend's to choose:
 * the program's text carries the `layout(set = ..., binding = ...)` qualifiers, and a pipeline built against
 * a different arrangement would read whichever bytes happened to be bound there. So `create` takes the
 * program's DECLARED bindings (api/ProgramAbi) and builds every set from them - dynamic uniform bindings for
 * the L1 blocks, one sampler per declared sampled input - plus the push ranges the text declares. A program
 * that declares nothing this backend can fill (a foreign block, an oversized or non-std140 block, an
 * unsupported sampler, a `skyMap` whose environment image has not landed) is REFUSED rather than compiled
 * against a layout nobody can feed.
 *
 * THE SAMPLED-INPUT SET. A key also says how many colour attachments the pass' declared inputs offer
 * (`sampled_color_count`), and those textures are bound in a set of their own - set 1, right after the block
 * set at 0 - with one combined image sampler per colour texture, readable from the vertex or the fragment
 * stage. A layout is built per count and kept, because the count IS part of the identity: a pipeline is
 * compiled against one descriptor set layout, so "how many sampled textures this pass binds" has to be a
 * fact of the pipeline, exactly as the old implementation's per-source shader sets were. A program that
 * declares a sampler in that set has to declare it at a binding the count covers (`acquire` checks that).
 *
 * TWO KINDS, TWO DESCRIPTOR ABIS (see core::DrawKind). A CONTENT layer is the one above: the ABI's blocks at
 * set 0, the sampled inputs at set 1, vertex streams declared. A FULL-SCREEN layer is the other drawing
 * call the engine has: the source's colour attachments at bindings 0..N-1 of set 0 and NOTHING else - the
 * full-screen ABI the engine's own screen programs are written against (see
 * `vine::graphics::BuiltinShaders`: `screenCopyProgram` declares its sampler as `layout(binding = i)`, with
 * no set qualifier, so set 0 is where a full-screen sampler has to live), the full-screen vertex stage
 * supplied by the backend (the engine's canonical triangle, built from `gl_VertexIndex`, with no vertex
 * buffer and no vertex-side constants), and one 128-byte push block whose layout the SDK's builtin screen
 * programs declare (`ambient` + `projparms` + three directional lights). The two are not interchangeable,
 * which is why the layer knows its kind and the key carries it: `acquire()` refuses a key of the other kind
 * rather than compiling a pipeline for an ABI the draw will not bind.
 *
 * NO DEVICE IS NEEDED. Nothing here compiles a Vulkan object: a `vsg::GraphicsPipeline` is a create-info
 * until a `vsg::Context` compiles it as part of a command graph, and the GLSL is compiled to SPIR-V in
 * process. That is what makes the identity arithmetic testable without a GPU - the GPU compiles later.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

// The entry whose channels one of the factories below describes (see api/ContentFacts; the
// definition would close a cycle - the tables name this class' `Shaders`).
struct GeometryFacts;

/** @brief Content pipelines: identity in, `vsg::GraphicsPipeline` out. */
class ContentPipeline
{
  public:
    /** @brief The set THIS backward's own arrangement puts a pass' sampled inputs in (the engine's programs
     *         declare their maps in their own sets instead - see api/ProgramAbi). A sampler declared in this
     *         set is served from the pass' inputs (one per binding, in declaration order); a sampler anywhere
     *         else is filled by the CALLER, from the source its NAME states (see api/ContentImages: a
     *         material's texture or the white fallback, the pass' resolved shadow map, ...). */
    static constexpr std::uint32_t kInputSet = 1U;

    /** @brief One vertex stream the pipeline declares. */
    struct VertexBinding
    {
        std::uint32_t binding{0};            ///< Binding index the array is bound at.
        std::uint32_t stride{0};             ///< Bytes between two elements.
        bool          per_instance{false};   ///< Instance rate instead of vertex rate.
    };

    /** @brief One attribute the pipeline declares. */
    struct VertexAttribute
    {
        std::uint32_t location{0};   ///< Shader location.
        std::uint32_t binding{0};    ///< Binding the value is read from.
        std::uint32_t format{0};     ///< `VkFormat` of the component.
        std::uint32_t offset{0};     ///< Byte offset inside the element.
    };

    /** @brief The GLSL text the pipeline's shader stages are compiled from, and the variant's defines. */
    struct Shaders
    {
        std::string              vertex;    ///< Vertex stage source.
        std::string              fragment;  ///< Fragment stage source.
        std::string              entry{"main"};  ///< Entry point of both stages.
        /// The defines this compile asks for (see api/ProgramVariant). The texts are the SAME text for
        /// every variant of a program - what changes is which of their `#pragma import_defines` names
        /// are defined, and vsg delivers a name only when the source asks for it AND the caller lists
        /// it. The ABI must be scanned with the same list (buildProgramFacts), or the layout and the
        /// module disagree about what is in effect.
        std::vector<std::string> defines;
    };

    /** @brief What the pipeline is built against, besides the identity. */
    struct Settings
    {
        std::uint32_t color_attachments{1};  ///< Colour attachments the blend state declares.
        std::uint32_t push_bytes{128};       ///< The FULL-SCREEN path's push budget: a content program's push
                                             ///< ranges are its own declarations (see `create`).
    };

    /** @brief What an acquire did. */
    struct Result
    {
        core::VariantPool::Action                action{core::VariantPool::Action::Created};  ///< Created or reused.
        std::uint64_t                            id{0};      ///< The variant id the pool handed out.
        ::vsg::ref_ptr<::vsg::GraphicsPipeline>   pipeline;  ///< The pipeline (null when it could not be built).
    };

  public:
    /** @brief Creates the layer for a CONTENT program: the layout IS what the program's text declares.
     *
     * The bindings come from the program's own `layout(set = ..., binding = ...)` qualifiers (see
     * api/ProgramAbi): every L1 block the text declares becomes a DYNAMIC uniform binding at the (set,
     * binding) the text names, in the set the text puts it in, and the push ranges are the text's own (offset,
     * size, stage). The canonical arrangement (the five blocks at set 0's bindings 0..4) is therefore not a
     * constant of this class: it is what this backend's own programs happen to declare. A caller that binds
     * these pipelines builds its block sets for the same shape - `blockShape()` answers it - so the set it
     * binds and the layout the pipeline was compiled against cannot be separately defined.
     *
     * WHAT IT REFUSES (the layer cannot describe it, so no pipeline can be built):
     *   * a FOREIGN block (a uniform block whose type name is none of the five L1 names - nothing can fill it);
     *   * a role block whose declared layout is not `std140` (its size is then the compiler's, and the bytes
     *     this backend binds are std140 structs);
     *   * a role block that declares MORE bytes than the L1 struct carries (the bytes past the struct are not
     *     the ABI's: the block is bound with a range that ends there);
     *   * an `OtherSampler` (the images this backend binds are float 2D and cube views);
     *   * a program that samples the frame's environment (`skyMap`): that image has not landed, so there is no
     *     descriptor to declare for it;
     *   * a sampler binding the key's input count does not cover (checked in @ref acquire, where the count is
     *     known); a sampler in any OTHER set is the caller's to fill, and the caller's to get wrong.
     *
     * @param abi        The bindings and push ranges the program's text declares.
     * @param bindings   Vertex stream bindings the pipeline declares.
     * @param attributes Vertex attributes the pipeline declares.
     * @param shaders    GLSL text of both stages.
     * @param settings   Colour attachment count (and the full-screen path's push budget).
     * @return The layer, or null when the GLSL did not compile or a declaration cannot be served (a layer
     *         that can never build a pipeline has nothing to offer, and the caller reports it rather than
     *         drawing an empty frame).
     */
    static std::unique_ptr<ContentPipeline> create(const ProgramAbi& abi,
                                                   std::span<const VertexBinding>  bindings,
                                                   std::span<const VertexAttribute> attributes,
                                                   const Shaders& shaders, const Settings& settings);

    /** @brief Creates the layer for a content program with the default settings (see @ref Settings).
     *
     * @param abi        The bindings and push ranges the program's text declares.
     * @param bindings   Vertex stream bindings the pipeline declares.
     * @param attributes Vertex attributes the pipeline declares.
     * @param shaders    GLSL text of both stages.
     * @return The layer, or null when the GLSL did not compile or a declaration cannot be served.
     */
    static std::unique_ptr<ContentPipeline> create(const ProgramAbi& abi,
                                                   std::span<const VertexBinding>  bindings,
                                                   std::span<const VertexAttribute> attributes,
                                                   const Shaders& shaders);

    /** @brief Creates the layer for a content program over one geometry ENTRY's channels.
     *
     * THE LAYOUT IS THE ENTRY'S OWN CHANNELS, spelled once: the binding a channel is fed at is the stream
     * store's canonical number (`StreamUploads::bindingOfCanonical` - what the draw binds by, NOT the
     * channel's order in the entry and not the shader's location, which is 8 for the reserved texcoord
     * slot), its location is the shader's own, and its format follows its component count (a channel's
     * scalars are 32-bit floats - see core::StreamKey). A builder that re-derived any of the three would
     * declare a pipeline the draw's streams do not match, and that is invisible until the picture is wrong.
     *
     * A CUSTOM channel (no canonical binding) is refused here (null): this backend's stream store has no
     * bind for one, so a pipeline playing it could never be fed (api/GeometryFacts describes such a
     * channel; the upload layer is where the limit is said).
     *
     * @param abi      The bindings and push ranges the program's text declares.
     * @param geometry The entry whose channels the pipeline declares.
     * @param shaders  GLSL text of both stages.
     * @param settings What the pipeline is built against besides the identity.
     * @return The layer, or null when the GLSL did not compile, a declaration cannot be served, or a channel
     *         has no canonical binding.
     */
    static std::unique_ptr<ContentPipeline> create(const ProgramAbi& abi, const GeometryFacts& geometry,
                                                   const Shaders& shaders, const Settings& settings);

    /** @brief Creates a FULL-SCREEN layer: the other descriptor ABI, whose set is what the text declares.
     *
     * The stages are the engine's canonical full-screen vertex stage plus the host's fragment stage (see
     * `api/ContentSources`: `buildScreenProgramFacts` composes exactly that pair), so the layer compiles one
     * program pair whose vertex stage generates the triangle from `gl_VertexIndex`. The bindings are the
     * program's own declarations (api/ProgramAbi) like a content layer's - with the full-screen ABI's three
     * rules, which `create` REFUSES a text for breaking:
     *   * everything lives in SET 0: the engine's screen programs write `layout(binding = i)` with no set
     *     qualifier, and its shadow ABI inserts its map and its block into that same set - there is no
     *     second set for this ABI to bind;
     *   * the only BLOCK it may declare is the shadow's (`VineShadowBlock`): a full-screen program's lights
     *     travel in its 128-byte push block, so any other L1 block names bytes nothing would write;
     *   * a sampler is a `sampler2D` and never the environment (`skyMap` - see api/ContentImages).
     *
     * WHERE THE IMAGES COME FROM is the pass', not a caller's: the source's colour attachments occupy
     * bindings 0..N-1 (the ABI's own convention: "binding i is attachment i"), the source's depth takes
     * binding N when a shader may sample it, a declared `shadow_map` takes ITS binding, and a declared
     * shadow block takes its own. `sampledSetLayout` answers that layout per (colours, depths) - the pass
     * builds the set from it (see ContentPass), which is why `declaredSets()` is empty for this kind.
     *
     * @param abi      The bindings and push ranges the program's text declares.
     * @param shaders  GLSL text of both stages (full-screen vertex + screen fragment).
     * @param settings Colour attachment count and push budget.
     * @return The layer, or null when the GLSL did not compile or a declaration breaks one of the rules
     *         above (a layer that can never build a pipeline has nothing to offer).
     */
    static std::unique_ptr<ContentPipeline> createScreen(const ProgramAbi& abi, const Shaders& shaders,
                                                         const Settings& settings);

    /** @brief Creates a full-screen layer with the default settings (see @ref Settings).
     *
     * @param abi     The bindings and push ranges the program's text declares.
     * @param shaders GLSL text of both stages (full-screen vertex + screen fragment).
     * @return The layer, or null when the GLSL did not compile or a declaration cannot be served.
     */
    static std::unique_ptr<ContentPipeline> createScreen(const ProgramAbi& abi, const Shaders& shaders);

    ~ContentPipeline();

    ContentPipeline(const ContentPipeline&) = delete;
    ContentPipeline& operator=(const ContentPipeline&) = delete;

  public:
    /** @brief Gets which drawing call this layer compiles pipelines for (see core::DrawKind). */
    [[nodiscard]] core::DrawKind kind() const noexcept;

    /** @brief Gets the bindings and push ranges the layer was built from. */
    [[nodiscard]] const ProgramAbi& abi() const noexcept;

    /** @brief Gets the block shape the program declares in @p set (empty when it declares none there).
     *
     * This is what a caller builds its block set from: `BlockDescriptors::create(device, storage,
     * layer->blockShape(set), set)` produces the set whose layout the pipelines of this layer were compiled
     * against, because both are the same shape (see api/BlockDescriptors).
     *
     * @param set Descriptor set index.
     * @return The shape, in binding order.
     */
    [[nodiscard]] std::span<const BlockDescriptors::Binding> blockShape(std::uint32_t set) const noexcept;

    /** @brief Gets the sampled bindings the program declares in @p set, ascending (empty when it declares
     *         none there).
     *
     * The other half of @ref blockShape: the ENGINE's own set 0 carries the material block AND the diffuse
     * map, and a set the caller builds for such a program must carry an image at exactly these bindings -
     * the layout this layer compiled its pipelines against declares them, so a set without them (or with
     * images at bindings the text never names) is refused rather than bound.
     *
     * @param set Descriptor set index.
     * @return The sampled bindings, ascending.
     */
    [[nodiscard]] std::span<const std::uint32_t> samplerBindings(std::uint32_t set) const noexcept;

    /** @brief Gets the set indices the program declares ANYTHING in (blocks and/or sampled images),
     *         ascending: the sets a caller has to build and a pass has to bind.
     *
     * EMPTY for a FULL-SCREEN layer, and that is not "nothing to bind": its set is the program's own
     * declaration and the PASS builds it (see @ref sampledSetLayout) - a caller that built one would have to
     * know which of the pass' images each binding reads, which is the pass' own bookkeeping.
     */
    [[nodiscard]] std::span<const std::uint32_t> declaredSets() const noexcept;

    /** @brief Gets (or builds) the pipeline for an identity.
     *
     * @param pool The scope's variant pool: it answers whether an identity is already compiled.
     * @param key  Pipeline identity (identity layer only - see the file note; its kind must be this layer's).
     * @return What the pool answered and the pipeline (null when the object could not be built).
     */
    [[nodiscard]] Result acquire(core::VariantPool& pool, const core::PipelineKey& key);

  public:
    /** @brief Gets the layout a layer of this kind binds when it samples nothing.
     *
     * A content layer always has one (the block set alone): a pass that reads no input still binds its blocks.
     * A full-screen layer's is the set its TEXT declares - the shadow map's binding and its block's - or the
     * push alone when the text declares nothing (a screen program that shades from its push block is a legal
     * call).
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::PipelineLayout> layout() const noexcept;

    /** @brief Gets (or builds and keeps) the pipeline layout that binds @p sampled_color_bindings colour
     *         samplers and @p sampled_depth_bindings depth samplers.
     *
     * The counts are the key's (`PipelineKey::sampled_color_count` / `sampled_depth_count`), so every pipeline
     * this layer holds was compiled against the layout this call answers for its own pair. The sampled set's
     * INDEX is the layer's kind's: set 1 after the block set for a content layer, set 0 for a full-screen one.
     *
     * @param sampled_color_bindings Number of colour textures the pass binds.
     * @param sampled_depth_bindings Number of depth textures the pass binds (they follow the colours, one per
     *                               input that offers a sampleable depth - see core::CompiledInput; for a
     *                               FULL-SCREEN layer it is the source's own depth, since the shadow map
     *                               takes the binding its text names - see @ref sampledSetLayout).
     * @return The layout, or null when it could not be created (or does not exist for this kind).
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::PipelineLayout> layoutFor(std::uint32_t sampled_color_bindings,
                                                                 std::uint32_t sampled_depth_bindings);

    /** @brief Gets (or builds and keeps) the sampled-input set layout for @p color_bindings colour samplers
     *         and @p depth_bindings depth samplers.
     *
     * The caller that binds the images uses THIS object, so the set it builds is laid out exactly like the
     * set the pipelines of this layer expect.
     *
     * A CONTENT layer's binding order is the inputs' declaration order - each input's colour attachments in
     * attachment order, then its depth - so a shader can name a binding once and keep reading the same thing
     * as long as the pass declares its inputs in the same order.
     *
     * A FULL-SCREEN layer's is the engine's screen ABI plus whatever the text declares: the source's colour
     * attachments at bindings 0..color_bindings-1 (binding i IS attachment i), the source's depth at the next
     * binding, and then the text's own declarations - a `shadow_map` at the binding the text names (not at
     * "the next free slot": the engine's shadowed lighting program hard-codes 5 for a four-colour G-buffer,
     * so a pass whose source depth is NOT sampleable would otherwise put the map one binding too low), and a
     * declared shadow block at its own binding. A depth slot never takes a binding the text names for
     * something else.
     *
     * @param color_bindings Number of combined image samplers (colour textures).
     * @param depth_bindings Number of combined image samplers (DEPTH textures the pass binds itself: one per
     *                       input that offers a sampleable depth, minus the shadow map for a full-screen
     *                       layer - that one takes its declared binding).
     * @return The set layout, or null for zero bindings (a layer with nothing to sample and nothing declared
     *         has no such set).
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::DescriptorSetLayout> sampledSetLayout(std::uint32_t color_bindings,
                                                                             std::uint32_t depth_bindings);

    /** @brief Gets the sampler the sampled-input set binds its IMAGES with.
     *
     * One per layer, created on first use: the images of a declared input are colour attachments, so the
     * default sampler (linear, repeat) is the right thing to interpolate a picture with.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::Sampler> inputSampler();

    /** @brief Gets the sampler the sampled-input set binds a DEPTH texture with.
     *
     * NEAREST, for the reason the engine's own shadow code gives: a depth-sampling shader compares exact
     * depths (the bias and the comparison are the shader's), so filtering across texels would invent a depth
     * nobody rasterised. Separate from the colour sampler because it is a different question.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::Sampler> depthSampler();

    /** @brief Gets the compiled shader stages. */
    [[nodiscard]] const ::vsg::ShaderStages& stages() const noexcept;

    /** @brief Gets the number of pipelines this layer holds. */
    [[nodiscard]] std::size_t pipelines() const noexcept;

    /** @brief Gets the number of pipelines built. */
    [[nodiscard]] std::uint64_t compiles() const noexcept;

    /** @brief Gets the number of acquires that could not build an object. */
    [[nodiscard]] std::uint64_t failures() const noexcept;

    /** @brief Checks that this layer holds no pipeline the pool has forgotten.
     *
     * The two are written at different places - an id comes from the pool, an object from this layer - so a
     * bug can leave an object alive that nothing can ever look up again. The check is one-directional on
     * purpose: a pool id without an object here is a build that failed, which is counted separately.
     *
     * @param pool The pool the identities came from.
     * @return true when every pipeline this layer holds still has a live id.
     */
    [[nodiscard]] bool agreesWithPool(const core::VariantPool& pool) const noexcept;

  private:
    struct Data;
    // Lexically after Data so the out-of-line destructor is the only place that needs the complete type.
    std::unique_ptr<Data> d;

    ContentPipeline();
};

V_VSG_NS_END
