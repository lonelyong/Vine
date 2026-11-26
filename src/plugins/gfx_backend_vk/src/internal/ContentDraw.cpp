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
 * @brief Answers whether two viewports state the same rectangle and depth range.
 *
 * @param left  One viewport.
 * @param right The other.
 * @return true when recording either one would leave the command buffer in the same state.
 */
[[nodiscard]] bool sameViewport(const VkViewport& left, const VkViewport& right) noexcept
{
    return left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height &&
           left.minDepth == right.minDepth && left.maxDepth == right.maxDepth;
}

/**
 * @brief Answers whether two scissors state the same rectangle.
 *
 * @param left  One scissor.
 * @param right The other.
 * @return true when recording either one would leave the command buffer in the same state.
 */
[[nodiscard]] bool sameScissor(const VkRect2D& left, const VkRect2D& right) noexcept
{
    return left.offset.x == right.offset.x && left.offset.y == right.offset.y && left.extent.width == right.extent.width &&
           left.extent.height == right.extent.height;
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
 *   (diag(1, -1, 1, 1) makes the picture upright in the device's top-left framebuffer space) and z is
 *   mapped to `0.5 - 0.5 z` (near -1 -> 1, far +1 -> 0, matching the depth the pass clears to). The winding this
 *   leaves is stated once, by measurement, in StateMapping::kFrontFace.
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
[[nodiscard]] detail::PushRangeConstants pushConstantsOf(const vn::graphics::CameraSnapshot& camera, const vn::math::Mat4d& model) noexcept
{
    detail::PushRangeConstants push{};
    copyMatrix(foldToDeviceClip(camera.projection), push.projection);
    copyMatrix(camera.view * model, push.model_view);
    return push;
}

/**
 * @brief Packs the shadow block one draw packet shades with (see ShaderAbi.hpp `VineShadowBlock`).
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
 * @param draw   The draw packet: its camera (the view space to map from) and the lights it announced.
 * @return The block; every field is written and the switch is off when the map cannot shade this call.
 */
[[nodiscard]] vn::graphics::VineShadowBlock buildShadowBlock(const ContentDraw::ShadowSource& shadow, const vn::graphics::DrawPacket& draw) noexcept
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

void ContentDraw::beginFrame() noexcept
{
    // A NEW COMMAND BUFFER HOLDS NOTHING (see the header): what the last frame set is not a fact about this one.
    recorded = RecordedState{};
}

std::uint64_t ContentDraw::stateCommands() const noexcept
{
    return state_commands_;
}

void ContentDraw::bindPipeline(const VkCommandBuffer commands, const VkPipeline pipeline) noexcept
{
    if (recorded.pipeline == pipeline) {
        return;
    }
    caches.table().vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    ++state_commands_;
    // THE STATE SURVIVES THE BIND, BECAUSE THE DYNAMIC SET IS A CONSTANT HERE. A bind resets no dynamic state, and a
    // value only stops being usable when the NEW pipeline does not declare that state. Every pipeline this backend
    // builds - content and full-screen alike - comes from `buildPipeline`, which declares one list of eleven dynamic
    // states, so every value the buffer holds is still a value the new pipeline reads: re-stating the rectangle after
    // each bind would be one command per item with no new meaning (the case `TwoAlternatingPipelinesRestateNoState`
    // pins it). An edit that gives a SECOND builder a narrower list must reset the shadow where it binds.
    recorded.pipeline = pipeline;
}

void ContentDraw::recordCallState(const VkCommandBuffer                          commands,
                                  const VkViewport&                              viewport,
                                  const VkRect2D&                                scissor,
                                  const vn::graphics::ResolvedCommandState&      resolved,
                                  const std::uint32_t                            color_attachments,
                                  const float                                    depth_bias_units,
                                  const float                                    depth_bias_factor) noexcept
{
    const VolkDeviceTable& table = caches.table();

    // THE RECTANGLE IS A PASS FACT (it is computed from the pass' draw size), so a pass with N draws used to state it N
    // times. Now it is stated when it differs - which, inside one pass, is never after the first draw.
    if (!recorded.viewport_set || !sameViewport(recorded.viewport, viewport)) {
        table.vkCmdSetViewport(commands, 0U, 1U, &viewport);
        recorded.viewport     = viewport;
        recorded.viewport_set = true;
        ++state_commands_;
    }
    if (!recorded.scissor_set || !sameScissor(recorded.scissor, scissor)) {
        table.vkCmdSetScissor(commands, 0U, 1U, &scissor);
        recorded.scissor     = scissor;
        recorded.scissor_set = true;
        ++state_commands_;
    }

    // THE DYNAMIC HALF IS THE SEAM'S VALUE (`DynamicState`), so a change to it lands in one place; the device's range is
    // this layer's clamp (a width above it is an invalid command).
    const vn::graphics::DynamicState dynamic    = vn::graphics::resolveDynamicState(resolved);
    const float                     line_width = std::clamp(dynamic.line_width, 1.0F, caches.lineWidthLimit());
    if (!recorded.line_width_set || recorded.line_width != line_width) {
        table.vkCmdSetLineWidth(commands, line_width);
        recorded.line_width     = line_width;
        recorded.line_width_set = true;
        ++state_commands_;
    }
    if (!recorded.depth_bias_set || recorded.depth_bias_units != depth_bias_units || recorded.depth_bias_factor != depth_bias_factor) {
        table.vkCmdSetDepthBias(commands, depth_bias_units, 0.0F, depth_bias_factor);
        recorded.depth_bias_units  = depth_bias_units;
        recorded.depth_bias_factor = depth_bias_factor;
        recorded.depth_bias_set    = true;
        ++state_commands_;
    }

    // THE RASTER HALF: cull, polygon mode, depth test/write/compare and blend - commands since `T54`, and each stated only
    // when it differs from what the buffer already holds.
    const VkPolygonMode polygon = StateMapping::polygonMode(resolved.state.polygonMode);
    if (!recorded.raster_set || recorded.polygon != polygon) {
        table.vkCmdSetPolygonModeEXT(commands, polygon);
        recorded.polygon = polygon;
        ++state_commands_;
    }
    const VkCullModeFlags cull = StateMapping::cullMode(resolved.state.cullMode);
    if (!recorded.raster_set || recorded.cull != cull) {
        table.vkCmdSetCullMode(commands, cull);
        recorded.cull = cull;
        ++state_commands_;
    }
    const VkBool32 depth_test = resolved.state.depth.test ? VK_TRUE : VK_FALSE;
    if (!recorded.raster_set || recorded.depth_test != depth_test) {
        table.vkCmdSetDepthTestEnable(commands, depth_test);
        recorded.depth_test = depth_test;
        ++state_commands_;
    }
    const VkBool32 depth_write = resolved.state.depth.write ? VK_TRUE : VK_FALSE;
    if (!recorded.raster_set || recorded.depth_write != depth_write) {
        table.vkCmdSetDepthWriteEnable(commands, depth_write);
        recorded.depth_write = depth_write;
        ++state_commands_;
    }
    const VkCompareOp depth_compare = StateMapping::compareOp(resolved.state.depth.compare);
    if (!recorded.raster_set || recorded.depth_compare != depth_compare) {
        table.vkCmdSetDepthCompareOp(commands, depth_compare);
        recorded.depth_compare = depth_compare;
        ++state_commands_;
    }
    recorded.raster_set = true;

    // A DEPTH-ONLY PASS HAS NO ATTACHMENT TO BLEND INTO, so it states no blend at all - the content rule for an MRT
    // target too (its attachments hold data, not a surface), which is why "enabled" is decided here rather than sent.
    if (color_attachments == 0U) {
        return;
    }
    const VkBool32      blend_enabled = color_attachments == 1U && resolved.state.blend.enabled ? VK_TRUE : VK_FALSE;
    const VkBlendFactor blend_src     = StateMapping::blendFactor(resolved.state.blend.src);
    const VkBlendFactor blend_dst     = StateMapping::blendFactor(resolved.state.blend.dst);
    if (recorded.blend_set && recorded.blend_enabled == blend_enabled && recorded.blend_src == blend_src && recorded.blend_dst == blend_dst &&
        recorded.blend_attachments == color_attachments) {
        return;
    }
    constexpr std::uint32_t kChunk = 8U;
    VkColorBlendEquationEXT equations[kChunk]{};
    for (VkColorBlendEquationEXT& equation : equations) {
        equation.srcColorBlendFactor = blend_src;
        equation.dstColorBlendFactor = blend_dst;
        equation.colorBlendOp        = VK_BLEND_OP_ADD;
        equation.srcAlphaBlendFactor = blend_src;
        equation.dstAlphaBlendFactor = blend_dst;
        equation.alphaBlendOp        = VK_BLEND_OP_ADD;
    }
    for (std::uint32_t first = 0; first < color_attachments; first += kChunk) {
        const std::uint32_t count = std::min(kChunk, color_attachments - first);
        VkBool32            flags[kChunk]{};
        for (std::uint32_t index = 0; index < count; ++index) {
            flags[index] = blend_enabled;
        }
        table.vkCmdSetColorBlendEnableEXT(commands, first, count, flags);
        table.vkCmdSetColorBlendEquationEXT(commands, first, count, equations);
        state_commands_ += 2U;
    }
    recorded.blend_enabled     = blend_enabled;
    recorded.blend_src         = blend_src;
    recorded.blend_dst         = blend_dst;
    recorded.blend_attachments = color_attachments;
    recorded.blend_set         = true;
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

ContentDraw::DrawScope ContentDraw::DrawScope::forPass(const VkCommandBuffer         commands,
                                                      const VkExtent2D              extent,
                                                      const FrameHead&              head,
                                                      const ShadowSource            shadow,
                                                      const vn::graphics::PassDraw& pass) noexcept
{
    DrawScope scope;
    scope.commands = commands;
    scope.head     = &head;
    scope.shadow   = shadow;
    // THE PASS' RECTANGLE, OR THE WHOLE TARGET (a pass without a viewport draws all of it) - computed ONCE for the
    // pass instead of once per draw (see the header: it is a pass fact, `T54`).
    if (pass.viewport.has_value() && pass.viewport->isValid()) {
        scope.viewport.x      = static_cast<float>(pass.viewport->x);
        scope.viewport.y      = static_cast<float>(pass.viewport->y);
        scope.viewport.width  = static_cast<float>(pass.viewport->width);
        scope.viewport.height = static_cast<float>(pass.viewport->height);
        scope.scissor.offset  = VkOffset2D{ pass.viewport->x, pass.viewport->y };
        scope.scissor.extent  = VkExtent2D{ static_cast<std::uint32_t>(pass.viewport->width), static_cast<std::uint32_t>(pass.viewport->height) };
    }
    else {
        scope.viewport.x      = 0.0F;
        scope.viewport.y      = 0.0F;
        scope.viewport.width  = static_cast<float>(extent.width);
        scope.viewport.height = static_cast<float>(extent.height);
        scope.scissor.offset  = VkOffset2D{ 0, 0 };
        scope.scissor.extent  = extent;
    }
    scope.viewport.minDepth = 0.0F;
    scope.viewport.maxDepth = 1.0F;
    if (pass.depth_range.has_value() && pass.depth_range->isValid()) {
        // The pass' depth BAND: min is the far end, max the near end (see DepthRange), which is exactly Vulkan's
        // minDepth/maxDepth pair under reverse-Z.
        const std::pair<float, float> depth = StateMapping::viewportDepth(pass.depth_range);
        scope.viewport.minDepth            = depth.first;
        scope.viewport.maxDepth            = depth.second;
    }
    return scope;
}

void ContentDraw::record(const DrawScope&                         scope,
                         const vn::graphics::DrawPacket&          draw,
                         const std::span<const FrameHead::DrawPlan> plans,
                         ContentRefusal* const                    refusal)
{
    ContentRefusal  local;
    ContentRefusal& out = refusal != nullptr ? *refusal : local;
    out.clear();

    if (!caches.ready() || !scratch.ready() || scope.head == nullptr || scope.commands == VK_NULL_HANDLE || draw.commands == nullptr) {
        return;
    }
    // NO CAMERA MEANS NO DRAW (see DrawPacket::camera): the pass issues nothing, and that is not a failure.
    if (!draw.view.camera.present) {
        return;
    }
    if (draw.kind != vn::graphics::DrawKind::Scene) {
        out.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::WrongPath),
                   vn::String(u8"vk: a full-screen draw reached the content path (it has its own recordFullscreen)"));
        return;
    }
    // ONE PLAN PER COMMAND IS THE FRAME HEAD'S CONTRACT (see FrameHead::DrawPlan): a caller that resolved only
    // some of them would record a draw nobody resolved, so this call refuses rather than guessing.
    if (plans.size() < draw.commands->size()) {
        out.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::PlanMissing),
                   vn::String(u8"vk: the frame head did not resolve every command of this draw packet"));
        return;
    }
    // THE 1x1 WHITE STAND-IN was made at the frame head (see ContentCaches::resolveTextures), outside the render
    // pass - the only place its copy is legal. A frame without one binds nothing, so it records nothing.
    if (draw.commands->empty()) {
        return; // nothing named, nothing drawn (not a refusal: the call is simply empty)
    }
    if (scope.head->fallback_view == VK_NULL_HANDLE) {
        out.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::StandInMissing),
                   vn::String(u8"vk: the 1x1 stand-in every absent texture slot binds could not be made"));
        return;
    }

    // THE PASS' RECTANGLE comes with the scope (see DrawScope::forPass): it is a pass fact, resolved once.
    const VkCommandBuffer commands = scope.commands;
    const FrameHead&      head     = *scope.head;
    const ShadowSource    shadow   = scope.shadow;
    const VkViewport&     viewport = scope.viewport;
    const VkRect2D&       scissor  = scope.scissor;

    const auto refuse = [&out](vn::graphics::DiagnosticCategory kind, const char8_t* const code, vn::String text) {
        out.refuse(kind, code, std::move(text));
    };

    for (std::size_t command_index = 0; command_index < draw.commands->size(); ++command_index) {
        const vn::graphics::DrawItem& command = (*draw.commands)[command_index];
        const FrameHead::DrawPlan&         plan    = plans[command_index];
        // A COMMAND THE HEAD COULD NOT RESOLVE IS SKIPPED, AND THE HEAD ALREADY SAID WHY (see planCommand): the
        // draw packet's refusal carries that reason, and a later command still records.
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

        bindPipeline(commands, plan.pipeline);
        // THE STATE, ONE SHADOW DIFF (see recordCallState): the pass' rectangle, the dynamic half and the raster half are
        // each stated when they differ from what this command buffer already holds - so a pass with N draws that share a
        // state states it once, and a pass whose draws differ pays exactly for the differences.
        const vn::graphics::DynamicState dynamic = vn::graphics::resolveDynamicState(resolved);
        recordCallState(commands,
                        viewport,
                        scissor,
                        resolved,
                        plan.color_attachments,
                        dynamic.polygon_offset.units,
                        dynamic.polygon_offset.factor);
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

        const detail::PushRangeConstants push = pushConstantsOf(draw.view.camera, command.model_matrix);
        caches.table().vkCmdPushConstants(commands,
                                        caches.contentPipelineLayout(),
                                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                        0U,
                                        sizeof(push),
                                        &push);

        // THE BIND LISTS ARE THE RECORDER'S, NOT THE CALL'S (see bind_buffers_ in the header): a pass binds once per
        // command, and two vectors built per command made the recording path's allocations grow with its draw count
        // (measured: the per-frame count in `vk-backend.md` §9.1, most of it small vectors). Cleared and refilled here,
        // their storage is allocated once - when the widest geometry this driver has seen binds for the first time.
        bind_buffers_.clear();
        bind_offsets_.clear();
        for (const VkBuffer stream : plan.vertex_streams) {
            bind_buffers_.push_back(stream);
            bind_offsets_.push_back(0U);
        }
        if (plan.instance_buffer != VK_NULL_HANDLE) {
            // THE INSTANCE STREAM is the LAST binding, matching the layout `ensureContentPipeline` built for it
            // (one binding, stride of a matrix, input rate INSTANCE).
            bind_buffers_.push_back(plan.instance_buffer);
            bind_offsets_.push_back(0U);
        }
        if (!bind_buffers_.empty()) {
            caches.table().vkCmdBindVertexBuffers(commands, 0U, static_cast<std::uint32_t>(bind_buffers_.size()), bind_buffers_.data(), bind_offsets_.data());
        }

        const std::uint32_t instance_count = plan.instance_buffer != VK_NULL_HANDLE ? command.instance_count : 1U;
        const std::uint32_t first_instance = plan.instance_buffer != VK_NULL_HANDLE ? command.first_instance : 0U;
        if (plan.index_buffer != VK_NULL_HANDLE) {
            caches.table().vkCmdBindIndexBuffer(commands, plan.index_buffer, 0U, VK_INDEX_TYPE_UINT32);
            caches.table().vkCmdDrawIndexed(commands, plan.index_count, instance_count, 0U, 0, first_instance);
        }
        else if (plan.vertex_count > 0U) {
            caches.table().vkCmdDraw(commands, plan.vertex_count, instance_count, 0U, first_instance);
        }
        else {
            refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::NothingToDraw),
                   vn::String(u8"vk: a draw named neither an index stream nor vertices"));
        }
    }
}

void ContentDraw::recordFullscreen(const DrawScope&                scope,
                                   const vn::graphics::DrawPacket& draw,
                                   const FrameHead::DrawPlan&      plan,
                                   const FullscreenSource          source,
                                   ContentRefusal* const           refusal)
{
    ContentRefusal  local;
    ContentRefusal& out = refusal != nullptr ? *refusal : local;
    out.clear();

    if (!caches.ready() || !scratch.ready() || scope.head == nullptr || scope.commands == VK_NULL_HANDLE) {
        return;
    }
    // The same contract a content draw follows: no camera names no picture.
    if (!draw.view.camera.present) {
        return;
    }
    if (source.views.empty()) {
        out.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::FullscreenNoSource),
                   vn::String(u8"vk: a full-screen draw named no source target, so there is nothing to sample"));
        return;
    }
    if (scope.head->fallback_view == VK_NULL_HANDLE) {
        out.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::StandInMissing),
                   vn::String(u8"vk: the 1x1 stand-in every absent texture slot binds could not be made"));
        return;
    }
    // THE HEAD'S REFUSAL IS THE DRAW'S REFUSAL: a plan without a pipeline was already reported by the frame loop.
    if (plan.pipeline == VK_NULL_HANDLE) {
        return;
    }
    const vn::graphics::ResolvedCommandState& resolved = plan.resolved;
    const VkPipeline                          pipeline = plan.pipeline;

    // THE SCOPE'S FACTS, NAMED ONCE (see DrawScope): the same command buffer, head, map and rectangle for every call
    // of the pass - the rectangle included, because it is a PASS fact.
    const VkCommandBuffer commands = scope.commands;
    const FrameHead&      head     = *scope.head;
    const ShadowSource    shadow   = scope.shadow;
    const VkViewport&     viewport = scope.viewport;
    const VkRect2D&       scissor  = scope.scissor;

    // THE SLOTS: binding N is the source's attachment N; a binding past the source's count reads the fallback (a
    // full-screen program only declares the bindings its own ABI states, so nothing reads them).
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
        return;
    }
    writeSlice(slice, vn::graphics::VineMaterialBlock{}, lights_block, vn::graphics::VineDrawBlock{}, shadow_block);

    const VkImageView shadow_view    = shadow.view != VK_NULL_HANDLE ? shadow.view : head.fallback_view;
    const VkSampler   shadow_sampler = head.shadow_sampler != VK_NULL_HANDLE ? head.shadow_sampler : head.fallback_sampler;
    const VkDescriptorSet set            = scratch.fullscreenSetFor(slots, shadow_view, shadow_sampler);
    if (set == VK_NULL_HANDLE) {
        out.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                   contentRefusalCode(ContentRefusalCause::DescriptorSetMissing),
                   vn::String(u8"vk: the descriptor cache could not hand out a set"));
        return;
    }

    bindPipeline(commands, pipeline);
    // THE SAME STATE PATH AS THE CONTENT DRAW (see recordCallState), except the bias: this draw is the canonical triangle
    // at the far plane, so it states zero rather than the pass' polygon offset.
    recordCallState(commands, viewport, scissor, resolved, plan.color_attachments, 0.0F, 0.0F);
    // The full-screen set's dynamic bindings are the lights (6) and the shadow block (8), in binding order.
    const std::uint32_t offsets[2]{ static_cast<std::uint32_t>(slice + scratch.lightsOffset()),
                                     static_cast<std::uint32_t>(slice + scratch.shadowOffset()) };
    caches.table().vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, caches.fullscreenPipelineLayout(), 0U, 1U, &set, 2U, offsets);
    // The full-screen triangle: three vertices generated from gl_VertexIndex, no buffer, no index stream.
    caches.table().vkCmdDraw(commands, 3U, 1U, 0U, 0U);
}

VN_BACKEND_VK_NS_END
