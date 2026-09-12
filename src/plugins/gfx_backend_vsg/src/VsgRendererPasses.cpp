#include <vine/vsg/VsgRenderer.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <vsg/app/View.h>
#include <vsg/lighting/Light.h>
#include <vsg/state/ViewportState.h>
#include <vsg/state/material.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/RenderPass.h>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/vsg/CameraBridge.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>

#include <vine/vsg/VsgUtils.hpp>
#include <vine/vsg/VsgBackendUtility.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>
#include <vine/vsg/VsgRecordOrder.hpp>
#include <vine/vsg/VsgTargetBookkeeping.hpp>

V_VSG_NS_BEGIN

// The pass protocol and the content slots: beginPass / endPass and the retained pass scope,
// retiring the passes that stopped executing, and the per-slot content draw.
//
// This translation unit is one of several that share a single free-function
// layer (VsgPipelineFactory.hpp / VsgBackendUtility.hpp); the directive keeps
// its call sites unqualified.
using namespace detail;

namespace
{

} // namespace

void VsgRenderer::beginPass(vine::raw_ptr<const vine::graphics::RenderPass> pass)
{
    if (state.pass_open) {
        // The engine runs one pass at a time; a nested beginPass means the
        // previous scope was never ended, so its announced state would silently
        // apply to the new pass. Report it and start clean.
        diagnostics.report(vine::graphics::DiagnosticSeverity::Warning,
                           vine::graphics::DiagnosticCategory::PassProtocolViolation,
                           u8"beginPass() while a pass scope is open: the open pass' request was dropped");
    }
    // A pass opens a CLEAN request: no pass may inherit what an earlier one
    // announced. The direct-driver path (no beginPass) keeps its queue instead.
    resetPassRequest();
    state.request.pass = pass;
    state.pass_open    = true;
    if (pass != nullptr) {
        // The pass owns its retained slot and counts as active this frame: a
        // pass that is not announced again next frame is retired (see
        // retireInactivePassSlots), which is what makes disabling it take effect.
        state.passes_active_this_frame.insert(pass);
        state.pass_protocol_used = true;
    }
}

bool VsgRenderer::isPassScopeOpen() const
{
    return state.pass_open;
}

void VsgRenderer::endPass()
{
    if (!state.pass_open) {
        // Reported because it means the pass protocol is out of step: the
        // state announced since the last endPass (or beginPass) had already
        // been dropped, so whatever the caller expected to apply did not.
        diagnostics.report(vine::graphics::DiagnosticSeverity::Warning,
                           vine::graphics::DiagnosticCategory::PassProtocolViolation,
                           u8"endPass() without an open pass scope: the announced request was already dropped");
    }
    // Close the scope: everything the pass announced is dropped here (including
    // scope attributes no draw call consumed), so nothing can apply to the next
    // pass.
    state.pass_open = false;
    resetPassRequest();
}

bool VsgRenderer::refuseDeadTargetAnnouncement(const char* call)
{
    bool report = false;
    if (!state.request.takeDeadTargetAnnouncement(report)) {
        return false;
    }
    // One report per release episode: the request's flag is cleared by the next
    // setRenderTarget() (and with the request when a scope opens or closes), so a caller
    // that keeps drawing on the same dead announcement is told once — with the fix, which
    // is the point of the message: the draw went nowhere, not into the window.
    if (report) {
        diagnostics.report(vine::graphics::DiagnosticSeverity::Warning,
                           vine::graphics::DiagnosticCategory::PassProtocolViolation,
                           formatDiagnostic(u8"%s: the announced render target was released while it was"
                                            u8" still announced, so the call was skipped instead of falling"
                                            u8" back to the window — announce the target again (or nullptr to"
                                            u8" draw into the window)",
                                            call));
    }
    return true;
}

void VsgRenderer::retireInactivePassSlots()
{
    if (!state.pass_protocol_used) {
        return; // direct driver (legacy keys): nothing is pass-owned
    }
    // A slot needs retiring when its pass did not execute this frame and its
    // view is still attached. Already-retired slots are skipped, so a pass that
    // stays disabled costs nothing per frame (no scan hit, no detach, no
    // repeated diagnostic).
    const auto needs_retire = [this](const SlotKey& key, bool detached) {
        return !detached && key.owner != nullptr &&
               state.passes_active_this_frame.count(key.owner) == 0;
    };
    // NO device wait here: this path DETACHES a view from its graph and keeps the
    // slot (the view, its node and the compiled pipelines stay referenced by the
    // slot), so nothing is destroyed and nothing a pending command buffer names
    // can go away. Disabling a pass is a per-frame host decision of an editor, and
    // stopping the device for it would be a stall for no lifetime reason.
    bool any = false;
    for (auto& entry : state.targets) {
        auto& t = entry.second;
        t.forEachSlot([&](const SlotKey& key, auto& slot, auto) {
            if (!needs_retire(key, slot.detached)) {
                return;
            }
            detail::detachSlotView(state, t, entry.first, key, slot.view);
            slot.detached = true;
            any          = true;
        });
    }
    if (!any) {
        return; // nothing changed: no re-order, no log line
    }
    // Dropping a view can remove a command-graph dependency edge.
    detail::reconcileOffscreenOrder(state);
    V_LOGI("[VsgRenderer] retired (detached) the retained view of pass(es) not active this frame");
}

void VsgRenderer::releasePass(vine::raw_ptr<const vine::graphics::RenderPass> pass)
{
    if (pass == nullptr) {
        return;
    }
    const vine::graphics::RenderPass* removed = pass;
    for (auto& entry : state.targets) {
        detail::erasePassFromTarget(state, entry.first, removed);
    }
    state.passes_active_this_frame.erase(removed);
    if (state.request.pass == removed) {
        state.request.pass = nullptr;
    }
    // Dropping a sampling slot can change the off-screen record order.
    detail::reconcileOffscreenOrder(state);
}

V_VSG_NS_END
