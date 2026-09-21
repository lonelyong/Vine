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
 * THE SAMPLED-INPUT SET. A key also says how many colour attachments the pass' declared inputs offer
 * (`sampled_color_count`), and those textures are bound in a set of their own - set 1, right after the block
 * set at 0 - with one combined image sampler per colour texture, readable from the vertex or the fragment
 * stage. A layout is built per count and kept, because the count IS part of the identity: a pipeline is
 * compiled against one descriptor set layout, so "how many sampled textures this pass binds" has to be a
 * fact of the pipeline, exactly as the old implementation's per-source shader sets were.
 *
 * NO DEVICE IS NEEDED. Nothing here compiles a Vulkan object: a `vsg::GraphicsPipeline` is a create-info
 * until a `vsg::Context` compiles it as part of a command graph, and the GLSL is compiled to SPIR-V in
 * process. That is what makes the identity arithmetic testable without a GPU - the GPU compiles later.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

/** @brief Content pipelines: identity in, `vsg::GraphicsPipeline` out. */
class ContentPipeline
{
  public:
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

    /** @brief The GLSL text the pipeline's shader stages are compiled from. */
    struct Shaders
    {
        std::string vertex;     ///< Vertex stage source.
        std::string fragment;   ///< Fragment stage source.
        std::string entry{"main"};  ///< Entry point of both stages.
    };

    /** @brief What the pipeline is built against, besides the identity. */
    struct Settings
    {
        std::uint32_t color_attachments{1};  ///< Colour attachments the blend state declares.
        std::uint32_t push_bytes{128};       ///< Push-constant budget (0 = none), per the ABI.
    };

    /** @brief What an acquire did. */
    struct Result
    {
        core::VariantPool::Action                action{core::VariantPool::Action::Created};  ///< Created or reused.
        std::uint64_t                            id{0};      ///< The variant id the pool handed out.
        ::vsg::ref_ptr<::vsg::GraphicsPipeline>   pipeline;  ///< The pipeline (null when it could not be built).
    };

  public:
    /** @brief Compiles the shader stages and prepares the states and the layout.
     *
     * @param block_set  The block descriptor set layout every content pipeline binds.
     * @param bindings   Vertex stream bindings the pipeline declares.
     * @param attributes Vertex attributes the pipeline declares.
     * @param shaders    GLSL text of both stages.
     * @param settings   Colour attachment count and push budget.
     * @return The layer, or null when the GLSL did not compile (a layer that can never build a pipeline has
     *         nothing to offer, and the caller reports it rather than drawing an empty frame).
     */
    static std::unique_ptr<ContentPipeline> create(const ::vsg::ref_ptr<::vsg::DescriptorSetLayout>& block_set,
                                                   std::span<const VertexBinding>  bindings,
                                                   std::span<const VertexAttribute> attributes,
                                                   const Shaders& shaders, const Settings& settings);

    /** @brief Compiles the shader stages with the default settings (see @ref Settings).
     *
     * @param block_set  The block descriptor set layout every content pipeline binds.
     * @param bindings   Vertex stream bindings the pipeline declares.
     * @param attributes Vertex attributes the pipeline declares.
     * @param shaders    GLSL text of both stages.
     * @return The layer, or null when the GLSL did not compile.
     */
    static std::unique_ptr<ContentPipeline> create(const ::vsg::ref_ptr<::vsg::DescriptorSetLayout>& block_set,
                                                   std::span<const VertexBinding>  bindings,
                                                   std::span<const VertexAttribute> attributes,
                                                   const Shaders& shaders);

    ~ContentPipeline();

    ContentPipeline(const ContentPipeline&) = delete;
    ContentPipeline& operator=(const ContentPipeline&) = delete;

  public:
    /** @brief Gets (or builds) the pipeline for an identity.
     *
     * @param pool The scope's variant pool: it answers whether an identity is already compiled.
     * @param key  Pipeline identity (identity layer only - see the file note).
     * @return What the pool answered and the pipeline (null when the object could not be built).
     */
    [[nodiscard]] Result acquire(core::VariantPool& pool, const core::PipelineKey& key);

  public:
    /** @brief Gets the layout every content pipeline of this layer shares (the one with no sampled inputs).
     *
     * The set layouts this layer owns (the block set it was built with, and one sampled-input set per count)
     * sit at indices 0 and 1 - a caller whose block set lives at another index is outside this contract.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::PipelineLayout> layout() const noexcept;

    /** @brief Gets (or builds and keeps) the pipeline layout that binds @p sampled_color_bindings samplers.
     *
     * The count is the key's (`PipelineKey::sampled_color_count`), so every pipeline this layer holds was
     * compiled against the layout this call answers for its own count.
     *
     * @param sampled_color_bindings Number of colour textures the pass binds (0 = the block set alone).
     * @return The layout, or null when it could not be created.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::PipelineLayout> layoutFor(std::uint32_t sampled_color_bindings);

    /** @brief Gets (or builds and keeps) the sampled-input set layout for @p color_bindings samplers.
     *
     * The caller that binds the images uses THIS object, so the set it builds is laid out exactly like the
     * set the pipelines of this layer expect.
     *
     * @param color_bindings Number of combined image samplers (binding i = the i-th colour texture).
     * @return The set layout, or null for zero bindings (there is no sampled set at all then).
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::DescriptorSetLayout> sampledSetLayout(std::uint32_t color_bindings);

    /** @brief Gets the sampler the sampled-input set binds its images with.
     *
     * One per layer, created on first use: the images of a declared input are colour attachments, so the
     * default sampler (linear, repeat) is the right thing to interpolate a picture with.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::Sampler> inputSampler();

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
