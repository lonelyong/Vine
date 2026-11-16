#include <vine/backend_vk/internal/ContentDraw.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <vine/backend_vk/internal/ContentCaches.hpp>
#include <vine/backend_vk/internal/ContentImages.hpp>
#include <vine/backend_vk/internal/FrameScratch.hpp>
#include <vine/backend_vk/internal/StateMapping.hpp>
#include <vine/graphics/DrawAbi.hpp>
#include <vine/graphics/LightAbi.hpp>
#include <vine/graphics/MaterialAbi.hpp>
#include <vine/graphics/ShaderAbi.hpp>

VN_BACKEND_VK_NS_BEGIN

namespace
{

using detail::kTextureSlots;

/**
 * @brief Copies a column-major double matrix into a column-major float array.
 *
 * @param source      The matrix (element (row, column) access).
 * @param destination The sixteen floats, column-major.
 */
void copyMatrix(const vn::math::Mat4d& source, std::array<float, 16>& destination) noexcept
{
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            destination[column * 4 + row] = static_cast<float>(source(row, column));
        }
    }
}

/**
 * @brief Folds the SDK's clip space into the device's: y down, and z into the [0, 1] reverse-Z range.
 *
 * @details
 * - The SDK's camera builds an OpenGL-style projection (y up, near at clip z = -1, far at +1) - it is
 *   backend-neutral - while Vulkan defines framebuffer coordinates with the origin at the TOP-left and a depth
 *   range of [0, 1] that this backend reads with the GREATER comparison (see StateMapping::compareOp): a nearer
 *   fragment must own the LARGER stored value.
 * - One matrix carries both parts, exactly as the reference backend's `foldToDeviceClip` does: y is negated
 *   (diag(1, -1, 1, 1) keeps the picture and the front-face rule right, see StateMapping::kFrontFace) and z is
 *   mapped to `0.5 - 0.5 z` (near -1 -> 1, far +1 -> 0, matching the depth the pass clears to).
 * - FOLDING ONLY Y WAS A DEPTH BUG: the fragments' stored depths kept the OpenGL direction, so with GREATER the
 *   FARTHER fragment won whenever two overlapped. This backend's own tests had stated their depths in the folded
 *   direction, which is why they could not see it; the parity suite, which hands the SAME frame to both backends,
 *   did (M7).
 *
 * @param projection The camera's clip-space projection.
 * @return The same projection in the device's clip convention.
 */
[[nodiscard]] vn::math::Mat4d foldToDeviceClip(const vn::math::Mat4d& projection) noexcept
{
    vn::math::Mat4d fold{}; // identity: x and w are the same in both conventions
    fold(1, 1) = -1.0;      // the device's NDC is y-down
    fold(2, 2) = -0.5;      // reverse-Z: the SDK's near (-1) becomes 1, its far (+1) becomes 0
    fold(2, 3) = 0.5;
    return fold * projection;
}

/**
 * @brief Fills the push range from a pass' camera and one drawable's model matrix.
 *
 * @param camera The pass' camera snapshot (its projection is folded, see foldToDeviceClip).
 * @param model  The drawable's world transform.
 * @return The range's bytes.
 */
[[nodiscard]] detail::ContentPushConstants pushConstantsOf(const vn::graphics::CameraSnapshot& camera, const vn::math::Mat4d& model) noexcept
{
    detail::ContentPushConstants push{};
    copyMatrix(foldToDeviceClip(camera.projection), push.projection);
    copyMatrix(camera.view * model, push.model_view);
    return push;
}

/**
 * @brief Packs the shadow block one drawing call shades with (see ShaderAbi.hpp `VineShadowBlock`).
 *
 * @details
 * - THE MAP'S IDENTITY IS SOMEBODY ELSE'S STATEMENT: the target says whose shadow it is (`RenderTarget::shadowOf`) and
 *   the producer published how to read it (`setProducerViewProjection`). This function only checks whether that map can
 *   shade THIS call - it never infers a map from an input's depth.
 * - FOUR FACTS DECIDE THE SWITCH, and each of them can turn it off on its own: no map, no published matrix, no view to
 *   map from, or a map whose light this call did not announce as an enabled caster of a kind that has a map
 *   (directional or spot). The light must also have taken a slot in the light block, because `params.w` names the
 *   light whose term the map scales - a slot the block cannot carry would scale somebody else's.
 * - THE MATRIX IS `light clip <- world` TIMES `world <- this view`: a fragment's view position is what the shading
 *   has, and the map's space is where it must be compared. It stays in the SDK's clip convention (y up, z in [-1, 1]):
 *   the shader's own conversion to the map's texel space and reverse-Z depth is the ABI's statement, and a matrix
 *   folded here would be a second one.
 * - `params` is {enabled, the casting light's OWN bias, strength 1, its slot}: the bias comes from `ShadowSettings`
 *   (a private default would be the second answer this ABI exists to prevent), and the SDK has no strength knob yet.
 *
 * @param shadow The map this pass samples (see ContentDraw::ShadowSource), or an empty one for none.
 * @param draw   The drawing call: its camera (the view space to map from) and the lights it announced.
 * @return The block; every field is written and the switch is off when the map cannot shade this call.
 */
[[nodiscard]] vn::graphics::VineShadowBlock buildShadowBlock(const ContentDraw::ShadowSource& shadow, const vn::graphics::DrawDescription& draw) noexcept
{
    vn::graphics::VineShadowBlock block{};
    if (shadow.light == nullptr || !shadow.has_view_projection || shadow.view == VK_NULL_HANDLE || !draw.view.camera.present ||
        draw.light_values == nullptr)
    {
        return block;
    }

    const std::span<const vn::graphics::LightValue> announced = draw.light_values->values();
    const vn::graphics::LightValue*                caster    = nullptr;
    for (const vn::graphics::LightValue& light : announced) {
        if (light.identity == shadow.light) {
            caster = &light;
            break;
        }
    }
    if (caster == nullptr || !caster->enabled || !caster->cast_shadow ||
        (caster->type != vn::graphics::LightType::Directional && caster->type != vn::graphics::LightType::Spot))
    {
        return block;
    }

    const std::size_t slot = vn::graphics::packedSlotOf(announced, shadow.light);
    if (slot >= vn::graphics::kMaxLights) {
        return block;
    }

    const vn::math::Mat4d view_to_light = shadow.view_projection * draw.view.camera.view.inverted();
    copyMatrix(view_to_light, block.view_to_light);
    block.params = { 1.0F, caster->shadow_bias, 1.0F, static_cast<float>(slot) };
    return block;
}

} // namespace

ContentDraw::ContentDraw(ContentCaches& device_caches, FrameScratch& frame_scratch) noexcept
  : caches(device_caches)
  , scratch(frame_scratch)
{
}

void ContentDraw::writeSlice(const VkDeviceSize                    slice,
                             const vn::graphics::VineMaterialBlock& material,
                             const vn::graphics::VineLightsBlock&   lights,
                             const vn::graphics::VineDrawBlock&     draw,
                             const vn::graphics::VineShadowBlock&   shadow) const noexcept
{
    std::byte* const bytes = scratch.slicePointer(slice);
    if (bytes == nullptr) {
        return;
    }
    std::memcpy(bytes, &material, sizeof(material));
    std::memcpy(bytes + scratch.lightsOffset(), &lights, sizeof(lights));
    std::memcpy(bytes + scratch.drawOffset(), &draw, sizeof(draw));
    // THE SHADOW BLOCK is the call's own statement (see buildShadowBlock); its `params.x = 0` is the engine's "no
    // shadow reached this pass" switch, which is what a pass that samples no map states.
    std::memcpy(bytes + scratch.shadowOffset(), &shadow, sizeof(shadow));
}

std::uint64_t ContentDraw::record(VkCommandBuffer                              commands,
                                  int                                          width,
                                  int                                          height,
                                  const vn::graphics::DrawDescription&         draw,
                                  const vn::graphics::PassDraw&                pass,
                                  const std::span<const FrameHead::DrawPlan>   plans,
                                  const FrameHead&                             head,
                                  ShadowSource                                 shadow,
                                  ContentRefusal* const                        refusal)
{
    ContentRefusal  local;
    ContentRefusal& out = refusal != nullptr ? *refusal : local;
    out.clear();

    if (!caches.ready() || !scratch.ready() || draw.commands == nullptr) {
        return 0;
    }
    // NO CAMERA MEANS NO DRAW (see DrawDescription::camera): the pass issues nothing, and that is not a failure.
    if (!draw.view.camera.present) {
        return 0;
    }
    if (draw.kind != vn::graphics::DrawDescription::Kind::Scene) {
        out.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::WrongPath),
                   vn::String(u8"vk: a screen draw reached the content path (it has its own recordScreen)"));
        return 0;
    }
    // ONE PLAN PER COMMAND IS THE FRAME HEAD'S CONTRACT (see FrameHead::DrawPlan): a caller that resolved only
    // some of them would record a draw nobody resolved, so this call refuses rather than guessing.
    if (plans.size() < draw.commands->size()) {
        out.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::PlanMissing),
                   vn::String(u8"vk: the frame head did not resolve every command of this drawing call"));
        return 0;
    }
    // THE 1x1 WHITE STAND-IN was made at the frame head (see ContentCaches::resolveTextures), outside the render
    // pass - the only place its copy is legal. A frame without one binds nothing, so it records nothing.
    if (draw.commands->empty()) {
        return 0; // nothing named, nothing drawn (not a refusal: the call is simply empty)
    }
    if (head.fallback_view == VK_NULL_HANDLE) {
        out.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::StandInMissing),
                   vn::String(u8"vk: the 1x1 stand-in every absent texture slot binds could not be made"));
        return 0;
    }

    // The rectangle the pass states, or the whole target (a pass without a viewport draws all of it).
    VkViewport viewport{};
    VkRect2D   scissor{};
    if (pass.viewport.has_value() && pass.viewport->isValid()) {
        viewport.x        = static_cast<float>(pass.viewport->x);
        viewport.y        = static_cast<float>(pass.viewport->y);
        viewport.width    = static_cast<float>(pass.viewport->width);
        viewport.height   = static_cast<float>(pass.viewport->height);
        scissor.offset    = VkOffset2D{ pass.viewport->x, pass.viewport->y };
        scissor.extent    = VkExtent2D{ static_cast<std::uint32_t>(pass.viewport->width), static_cast<std::uint32_t>(pass.viewport->height) };
    }
    else {
        viewport.x      = 0.0F;
        viewport.y      = 0.0F;
        viewport.width  = static_cast<float>(width);
        viewport.height = static_cast<float>(height);
        scissor.offset  = VkOffset2D{ 0, 0 };
        scissor.extent  = VkExtent2D{ static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height) };
    }
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;
    if (pass.depth_range.has_value() && pass.depth_range->isValid()) {
        // The pass' depth BAND: min is the far end, max the near end (see DepthRange), which is exactly Vulkan's
        // minDepth/maxDepth pair under reverse-Z.
        const std::pair<float, float> depth = StateMapping::viewportDepth(pass.depth_range);
        viewport.minDepth                  = depth.first;
        viewport.maxDepth                  = depth.second;
    }

    std::uint64_t recorded       = 0;
    VkPipeline    bound_pipeline = VK_NULL_HANDLE;

    const auto refuse = [&out](vn::graphics::DiagnosticCategory kind, const char8_t* const code, vn::String text) {
        out.refuse(kind, code, std::move(text));
    };

    for (std::size_t command_index = 0; command_index < draw.commands->size(); ++command_index) {
        const vn::graphics::RenderCommand& command = (*draw.commands)[command_index];
        const FrameHead::DrawPlan&         plan    = plans[command_index];
        // A COMMAND THE HEAD COULD NOT RESOLVE IS SKIPPED, AND THE HEAD ALREADY SAID WHY (see planDraw): the
        // drawing call's refusal carries that reason, and a later command still records.
        if (plan.pipeline == VK_NULL_HANDLE) {
            continue;
        }
        const vn::graphics::ResolvedCommandState& resolved = plan.resolved;

        // THE MATERIAL: resolved by the plan (the same answer the variant was derived from), so the slots bound and
        // the program compiled cannot describe two different materials.
        const vn::graphics::MaterialParameters* material_values = plan.material != nullptr ? &plan.material->values() : nullptr;

        // THE SLOTS: the frame head resolved each one (uploaded view or the stand-in), and the presence bits are the
        // shading's own switch - a slot the cache could not serve stays absent rather than reading stale bytes.
        std::array<VkDescriptorImageInfo, kTextureSlots> slots{};
        std::uint32_t                                    texture_flags = 0U;
        for (std::uint32_t slot_index = 0; slot_index < kTextureSlots; ++slot_index) {
            const FrameHead::SlotBinding& binding = plan.slots[slot_index];
            slots[slot_index].sampler     = binding.sampler;
            slots[slot_index].imageView   = binding.view;
            slots[slot_index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (binding.present) {
                texture_flags |= 1U << slot_index;
            }
        }

        // THE LIGHTS: the draw's announcement, packed in view space (see LightAbi.hpp's packLightBlock). No
        // announcement and an empty one both mean the backend default, an ambient-lit scene.
        vn::graphics::VineLightsBlock lights_block{};
        if (draw.light_values != nullptr) {
            (void)vn::graphics::packLightBlock(draw.light_values->values(), draw.view.camera, lights_block);
        }
        else {
            (void)vn::graphics::packLightBlock({}, draw.view.camera, lights_block);
        }

        // THE SHADOW: the map the pass samples becomes a block for THIS call (the caster's slot, its bias and the
        // matrix that maps a fragment's view position into the producer's clip - see buildShadowBlock).
        const vn::graphics::VineShadowBlock shadow_block = buildShadowBlock(shadow, draw);

        // ONE SLICE OF THE FRAME'S RING, WRITTEN BY OFFSET (see UniformRing): no buffer is created for this draw, and
        // a frame that does not fit skips the draw and grows the ring before the next one.
        VkDeviceSize slice = 0;
        if (!scratch.reserveSlice(slice)) {
            refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::UniformRingFull),
                   vn::String(u8"vk: the frame's uniform ring is full; this draw was skipped and the ring grows before the next frame"));
            continue;
        }
        // The material block's DEFAULTS are the default material's values, so a call with no material packs nothing
        // (see packMaterialBlock).
        vn::graphics::VineMaterialBlock material_block{};
        if (material_values != nullptr) {
            vn::graphics::packMaterialBlock(*material_values, texture_flags, material_block);
        }
        vn::graphics::VineDrawBlock draw_block{};
        (void)vn::graphics::packDrawBlock(command.model_matrix, command.opacity, resolved.state.pointSize, draw_block);
        writeSlice(slice, material_block, lights_block, draw_block, shadow_block);

        const VkImageView shadow_view    = shadow.view != VK_NULL_HANDLE ? shadow.view : head.fallback_view;
        const VkSampler   shadow_sampler = head.shadow_sampler != VK_NULL_HANDLE
                                               ? head.shadow_sampler
                                               : head.fallback_sampler;
        const VkDescriptorSet content_set = scratch.contentSetFor(slots, shadow_view, shadow_sampler);
        const VkDescriptorSet draw_set    = content_set != VK_NULL_HANDLE ? scratch.drawSetFor() : VK_NULL_HANDLE;
        if (content_set == VK_NULL_HANDLE || draw_set == VK_NULL_HANDLE) {
            refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::DescriptorSetMissing),
                   vn::String(u8"vk: the descriptor cache could not hand out a set"));
            continue;
        }

        if (plan.pipeline != bound_pipeline) {
            caches.table().vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, plan.pipeline);
            bound_pipeline = plan.pipeline;
        }
        caches.table().vkCmdSetViewport(commands, 0U, 1U, &viewport);
        caches.table().vkCmdSetScissor(commands, 0U, 1U, &scissor);
        // A PASS' LINE-WIDTH SWITCH IS A REQUEST (see PassRenderState::lineWidth): the device's range is the answer,
        // and above it the command itself would be invalid.
        caches.table().vkCmdSetLineWidth(commands, std::clamp(resolved.state.lineWidth, 1.0F, caches.lineWidthLimit()));
        caches.table().vkCmdSetDepthBias(commands, resolved.state.polygonOffset.units, 0.0F, resolved.state.polygonOffset.factor);
        const VkDescriptorSet sets[2]{ content_set, draw_set };
        // THE SLICE REACHES THE SHADERS AS A DYNAMIC OFFSET, in binding order across both sets: material (0), lights
        // (6) and the shadow block (8) of set 0, then the draw block (0) of set 1.
        const std::uint32_t offsets[4]{ static_cast<std::uint32_t>(slice),
                                        static_cast<std::uint32_t>(slice + scratch.lightsOffset()),
                                        static_cast<std::uint32_t>(slice + scratch.shadowOffset()),
                                        static_cast<std::uint32_t>(slice + scratch.drawOffset()) };
        caches.table().vkCmdBindDescriptorSets(commands,
                                             VK_PIPELINE_BIND_POINT_GRAPHICS,
                                             caches.contentPipelineLayout(),
                                             0U,
                                             2U,
                                             sets,
                                             4U,
                                             offsets);

        const detail::ContentPushConstants push = pushConstantsOf(draw.view.camera, command.model_matrix);
        caches.table().vkCmdPushConstants(commands,
                                        caches.contentPipelineLayout(),
                                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                        0U,
                                        sizeof(push),
                                        &push);

        std::vector<VkBuffer>     vertex_buffers;
        std::vector<VkDeviceSize> vertex_offsets;
        vertex_buffers.reserve(plan.vertex_streams.size() + (plan.instance_buffer != VK_NULL_HANDLE ? 1U : 0U));
        vertex_offsets.reserve(vertex_buffers.capacity());
        for (const VkBuffer stream : plan.vertex_streams) {
            vertex_buffers.push_back(stream);
            vertex_offsets.push_back(0U);
        }
        if (plan.instance_buffer != VK_NULL_HANDLE) {
            // THE INSTANCE STREAM is the LAST binding, matching the layout `ensureContentPipeline` built for it
            // (one binding, stride of a matrix, input rate INSTANCE).
            vertex_buffers.push_back(plan.instance_buffer);
            vertex_offsets.push_back(0U);
        }
        if (!vertex_buffers.empty()) {
            caches.table().vkCmdBindVertexBuffers(commands, 0U, static_cast<std::uint32_t>(vertex_buffers.size()), vertex_buffers.data(), vertex_offsets.data());
        }

        const std::uint32_t instance_count = plan.instance_buffer != VK_NULL_HANDLE ? command.instance_count : 1U;
        const std::uint32_t first_instance = plan.instance_buffer != VK_NULL_HANDLE ? command.first_instance : 0U;
        if (plan.index_buffer != VK_NULL_HANDLE) {
            caches.table().vkCmdBindIndexBuffer(commands, plan.index_buffer, 0U, VK_INDEX_TYPE_UINT32);
            caches.table().vkCmdDrawIndexed(commands, plan.index_count, instance_count, 0U, 0, first_instance);
            ++recorded;
        }
        else if (plan.vertex_count > 0U) {
            caches.table().vkCmdDraw(commands, plan.vertex_count, instance_count, 0U, first_instance);
            ++recorded;
        }
        else {
            refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::NothingToDraw),
                   vn::String(u8"vk: a draw named neither an index stream nor vertices"));
        }
    }
    return recorded;
}

std::uint64_t ContentDraw::recordScreen(VkCommandBuffer                            commands,
                                        int                                        width,
                                        int                                        height,
                                        const vn::graphics::DrawDescription&       draw,
                                        const vn::graphics::PassDraw&              pass,
                                        const FrameHead::DrawPlan&                 plan,
                                        const FrameHead&                           head,
                                        ScreenSource                               source,
                                        ShadowSource                               shadow,
                                        ContentRefusal* const                      refusal)
{
    ContentRefusal  local;
    ContentRefusal& out = refusal != nullptr ? *refusal : local;
    out.clear();

    if (!caches.ready() || !scratch.ready() || commands == VK_NULL_HANDLE) {
        return 0;
    }
    // The same contract a content draw follows: no camera names no picture.
    if (!draw.view.camera.present) {
        return 0;
    }
    if (source.views.empty()) {
        out.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::ScreenNoSource),
                   vn::String(u8"vk: a screen draw named no source target, so there is nothing to sample"));
        return 0;
    }
    if (head.fallback_view == VK_NULL_HANDLE) {
        out.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::StandInMissing),
                   vn::String(u8"vk: the 1x1 stand-in every absent texture slot binds could not be made"));
        return 0;
    }
    // THE HEAD'S REFUSAL IS THE DRAW'S REFUSAL: a plan without a pipeline was already reported by the frame loop.
    if (plan.pipeline == VK_NULL_HANDLE) {
        return 0;
    }
    const vn::graphics::ResolvedCommandState& resolved = plan.resolved;
    const VkPipeline                          pipeline = plan.pipeline;

    // The rectangle the pass states, or the whole destination (a pass without a viewport draws all of it).
    VkViewport viewport{};
    VkRect2D   scissor{};
    if (pass.viewport.has_value() && pass.viewport->isValid()) {
        viewport.x      = static_cast<float>(pass.viewport->x);
        viewport.y      = static_cast<float>(pass.viewport->y);
        viewport.width  = static_cast<float>(pass.viewport->width);
        viewport.height = static_cast<float>(pass.viewport->height);
        scissor.offset  = VkOffset2D{ pass.viewport->x, pass.viewport->y };
        scissor.extent  = VkExtent2D{ static_cast<std::uint32_t>(pass.viewport->width), static_cast<std::uint32_t>(pass.viewport->height) };
    }
    else {
        viewport.x      = 0.0F;
        viewport.y      = 0.0F;
        viewport.width  = static_cast<float>(width);
        viewport.height = static_cast<float>(height);
        scissor.offset  = VkOffset2D{ 0, 0 };
        scissor.extent  = VkExtent2D{ static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height) };
    }
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;

    // THE SLOTS: binding N is the source's attachment N; a binding past the source's count reads the fallback (a
    // screen program only declares the bindings its own ABI states, so nothing reads them).
    std::array<VkDescriptorImageInfo, kTextureSlots> slots{};
    for (std::uint32_t slot_index = 0; slot_index < kTextureSlots; ++slot_index) {
        slots[slot_index].sampler     = head.fallback_sampler;
        slots[slot_index].imageView   = slot_index < source.views.size() ? source.views[slot_index] : head.fallback_view;
        slots[slot_index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    vn::graphics::VineLightsBlock lights_block{};
    if (draw.light_values != nullptr) {
        (void)vn::graphics::packLightBlock(draw.light_values->values(), draw.view.camera, lights_block);
    }
    else {
        (void)vn::graphics::packLightBlock({}, draw.view.camera, lights_block);
    }

    // THE SHADOW THIS CALL SHADES WITH: the map the pass resolved (the same resolution a content draw uses - see
    // Device::resolveShadow), packed into this call's block. An empty source packs the engine's "no shadow reaches
    // this pass" block (`params.x = 0`) and the set binds the white stand-in - which is what a full-screen pass that
    // declares the shadow ABI but resolved no map states.
    const vn::graphics::VineShadowBlock shadow_block = buildShadowBlock(shadow, draw);
    // THE SAME RING AND THE SAME RULE as the content path: the constants are written into a slice and bound as dynamic
    // offsets; the set itself depends only on the images it names.
    VkDeviceSize slice = 0;
    if (!scratch.reserveSlice(slice)) {
        out.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::UniformRingFull),
                   vn::String(u8"vk: the frame's uniform ring is full; this draw was skipped and the ring grows before the "
                              u8"next frame"));
        return 0;
    }
    writeSlice(slice, vn::graphics::VineMaterialBlock{}, lights_block, vn::graphics::VineDrawBlock{}, shadow_block);

    const VkImageView shadow_view    = shadow.view != VK_NULL_HANDLE ? shadow.view : head.fallback_view;
    const VkSampler   shadow_sampler = head.shadow_sampler != VK_NULL_HANDLE ? head.shadow_sampler : head.fallback_sampler;
    const VkDescriptorSet set        = scratch.screenSetFor(slots, shadow_view, shadow_sampler);
    if (set == VK_NULL_HANDLE) {
        out.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::DescriptorSetMissing),
                   vn::String(u8"vk: the descriptor cache could not hand out a set"));
        return 0;
    }

    caches.table().vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    caches.table().vkCmdSetViewport(commands, 0U, 1U, &viewport);
    caches.table().vkCmdSetScissor(commands, 0U, 1U, &scissor);
    caches.table().vkCmdSetLineWidth(commands, std::clamp(resolved.state.lineWidth, 1.0F, caches.lineWidthLimit()));
    // The pipeline states depth bias as DYNAMIC (the content path does the same), so a full-screen draw owes the
    // command buffer the values too - zero is "no offset".
    caches.table().vkCmdSetDepthBias(commands, 0.0F, 0.0F, 0.0F);
    // The screen set's dynamic bindings are the lights (6) and the shadow block (8), in binding order.
    const std::uint32_t offsets[2]{ static_cast<std::uint32_t>(slice + scratch.lightsOffset()),
                                     static_cast<std::uint32_t>(slice + scratch.shadowOffset()) };
    caches.table().vkCmdBindDescriptorSets(commands,
                                         VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         caches.screenPipelineLayout(),
                                         0U,
                                         1U,
                                         &set,
                                         2U,
                                         offsets);
    // The full-screen triangle: three vertices generated from gl_VertexIndex, no buffer, no index stream.
    caches.table().vkCmdDraw(commands, 3U, 1U, 0U, 0U);
    return 1;
}

VN_BACKEND_VK_NS_END
