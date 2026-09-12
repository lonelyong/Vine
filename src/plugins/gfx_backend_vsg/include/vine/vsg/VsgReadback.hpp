#pragma once

/**
 * @brief Reading a target's attachments back to the CPU.
 *
 * A readback is synchronous by contract: it submits ONE immediate transfer (barriers plus a
 * blit / a copy) and may not return before the GPU is finished with it. This unit owns both
 * halves of that — the submission and the host-visible memory it reads from — plus the
 * shared prologue every readback starts with, so the do-nothing guards, the queue choice and
 * the (generous) fence timeout exist once instead of at every call.
 *
 * The public entry points stay `RenderBackend::readColorBuffer` / `readDepthBuffer` (their
 * declarations, formats and failure semantics are documented in VsgRenderer.hpp); this is the
 * work behind them, which is why it can be reasoned about without a renderer and, for its
 * guard half, tested without a device (see tests/test_vsg/ReadbackTest.cpp).
 */

#include <vine/vsg/vsg_global.hpp>

#include <cstdint>
#include <vector>

#include <vsg/core/ref_ptr.h>
#include <vsg/vk/DeviceMemory.h>

#include <vine/graphics/RenderTarget.hpp>

#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgRendererState.hpp>

V_VSG_NS_BEGIN

namespace detail
{

/** @brief The built target entry a readback reads from, or null.
 *
 * The shared prologue of readColorBuffer / readDepthBuffer: the window session
 * and viewer must exist, the target must have been built (its attachments exist)
 * and have a usable size. It deliberately does NOT stop the device: both callers
 * check the format (and report why) before paying for the wait.
 *
 * @param state  Session the readback runs against.
 * @param target Target to read from (null = unsupported).
 * @return The entry, or null when this readback is unsupported.
 */
[[nodiscard]] const VsgRenderTargetEntry* readbackTarget(const VsgRendererState& state,
                                                        vine::graphics::RenderTarget* target);

/** @brief Records @p commands into a fresh command buffer and waits for it.
 *
 * Owning the submission here also keeps the do-nothing guards, the queue choice and the
 * (generous) fence timeout in one place, instead of repeating them at every readback.
 *
 * @param state    Session whose device the transfer is submitted on.
 * @param commands Command list to record and complete.
 * @return false when the session has no usable device (nothing to submit to).
 */
[[nodiscard]] bool submitOneShot(const VsgRendererState& state, const ::vsg::ref_ptr<::vsg::Commands>& commands);

/** @brief Allocates the memory a readback destination has to live in.
 *
 * Host-visible and host-coherent on purpose: the CPU reads the staging buffer
 * (or the LINEAR image a colour readback blits into) the moment the submission
 * above returns.
 *
 * @param state        Session whose physical device picks the memory type.
 * @param device       Device to allocate on.
 * @param requirements Requirements of the resource it will be bound to.
 * @return The allocation; the caller binds it to its resource.
 */
[[nodiscard]] ::vsg::ref_ptr<::vsg::DeviceMemory> hostVisibleMemory(const VsgRendererState& state, ::vsg::Device* device,
                                                                   const VkMemoryRequirements& requirements);

/** @brief Reads one of a target's colour attachments back into @p out_pixels.
 *
 * The attachment must have been attached by the host and the target built; the format is
 * checked before the device is stopped, and every reason to refuse is reported instead of
 * silently returning an empty buffer.
 *
 * The session comes by NON-const reference because a served readback stops the device, and
 * that stop is counted (VsgRetireRing::waitForIdle) — the counter is what keeps the waits
 * honest, so the readback pays into it like every other path.
 *
 * @param state      Session the readback runs against.
 * @param diagnostics Route a refusal is reported on.
 * @param target     Target to read from.
 * @param attachment Colour attachment index to read.
 * @param out_pixels Receives width * height * 4 RGBA bytes on success.
 * @return true when the pixels were read; false when the target or attachment is unusable.
 */
[[nodiscard]] bool readColorBuffer(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                                   vine::graphics::RenderTarget* target, int attachment,
                                   std::vector<std::uint8_t>& out_pixels);

/** @brief Reads back the depth attachment of an off-screen render target.
 *
 * Only the unambiguous depth formats are read (D32_SFLOAT as stored, D16_UNORM divided by
 * 65535); a packed D24_UNORM_S8_UINT target is reported as unsupported instead of guessing
 * which 24 of its 32 bits hold the depth, and a target that borrows its depth
 * (RenderTarget::shareDepth) reports false because its depth is read through the SOURCE.
 *
 * The session comes by NON-const reference because a served readback stops the device (see
 * readColorBuffer).
 *
 * @param state     Session the readback runs against.
 * @param diagnostics Route a refusal is reported on.
 * @param target    Target to read from.
 * @param out_depths Receives width * height values in [0, 1], row-major, on success.
 * @return true when the depth values were read; false when the target has no readable depth.
 */
[[nodiscard]] bool readDepthBuffer(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                                   vine::graphics::RenderTarget* target, std::vector<float>& out_depths);

} // namespace detail

V_VSG_NS_END
