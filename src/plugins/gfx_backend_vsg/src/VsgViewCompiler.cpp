#include <vine/vsg/VsgViewCompiler.hpp>

#include <cstdlib>
#include <vector>

#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/app/CompileManager.h>
#include <vsg/vk/Framebuffer.h>

#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgRendererState.hpp>
#include <vine/vsg/VsgUtils.hpp>

V_VSG_NS_BEGIN

namespace detail
{

::vsg::ref_ptr<::vsg::ResourceHints> compileManagerHints() noexcept
{
    // The session's manager is the viewer's own, and `Viewer::compile()` builds it from the hints it
    // was CALLED with (it keeps none). Nothing here passes any yet -- which is the point of the value
    // existing once: a session manager and a replacement one are built alike, or neither is.
    return {};
}

void renewCompileContexts(VsgRendererState& state)
{
    if (state.viewer == nullptr) {
        return;
    }
    // What the manager holds, and how much of it can still be used. A registration whose slot is gone
    // is waste; one whose slot is alive is work the replacement re-does (the slot registers and compiles
    // again, which the self-test measures at about 0.4 s of a 2.8 s run). Replacing is therefore worth it
    // only once the waste is at least the live weight -- which is also what keeps a target rebuilt every
    // frame from rebuilding the manager every frame, where one slot dies out of a dozen live ones.
    std::size_t served = 0;
    for (const auto& target_entry : state.targets) {
        for (const auto& slot_entry : target_entry.second.content_slots) {
            if (slot_entry.second.compile_manager_generation == state.compile_manager_generation) {
                ++served;
            }
        }
    }
    const std::size_t waste = state.compile_context_registrations - served;
    if (waste == 0 || waste < served) {
        return;
    }
    // The bump IS the invalidation: a slot whose generation is behind the session's registers again
    // on its next compile, and one already left behind by an earlier renewal is not affected twice.
    ++state.compile_manager_generation;
    // The same construction vsg performs for a viewer's own manager (see compileManagerHints).
    state.viewer->compileManager = ::vsg::CompileManager::create(*state.viewer, compileManagerHints());
    // The count describes the manager now in place, which this backend has registered nothing into yet.
    state.compile_context_registrations = 0;
}

bool incrementalCompileViews(VsgRendererState& state)
{
    auto compileManager = state.viewer->compileManager;
    if (compileManager == nullptr) {
        return false;
    }

    for (const PendingCompileView& pending : state.pending_compile_views) {
        const ::vsg::ref_ptr<::vsg::View>& view = pending.view;
        if (view == nullptr) {
            return false;
        }

        // The queue entry says which slot the view belongs to (it has one producer, see
        // PendingCompileView), so the compile context is looked up rather than searched for — but the
        // slot is still CHECKED to hold this view: a slot can be dropped between the queue push and
        // this compile, and a new target allocated at the recorded address must not have this view
        // compiled against ITS framebuffer. An entry whose slot is gone is simply dropped: nothing
        // records that view any more, so there is nothing to compile (this used to fall back to
        // compiling the whole scene).
        const auto target_entry = state.targets.find(pending.target);
        if (target_entry == state.targets.end()) {
            continue;
        }
        VsgRenderTargetEntry& owner = target_entry->second;
        const auto            slot_entry = owner.content_slots.find(pending.slot);
        if (slot_entry == owner.content_slots.end() || slot_entry->second.view != view) {
            continue;
        }
        ContentSlot& slot      = slot_entry->second;
        const bool   is_window = pending.target == nullptr;

        // Register the slot's (render pass + view) context once PER MANAGER: the pool's
        // pooled traversal was built by CompileManager::create(viewer, hints)
        // when the window graph was still empty, so without this the pool has
        // no context that matches this view and compile() would compile
        // nothing. A renewal replaces the manager (renewCompileContexts), which leaves every slot's
        // generation behind -- so a slot that has to compile again registers into the new manager here.
        if (slot.compile_manager_generation != state.compile_manager_generation) {
            ::vsg::CollectResourceRequirements collect;
            view->accept(collect);
            const auto& requirements = collect.requirements;
            try {
                if (is_window) {
                    if (state.window == nullptr) {
                        return false;
                    }
                    compileManager->add(*state.window, view, requirements);
                }
                else {
                    // The compile context carries the render pass the pipeline
                    // is built against, so it must be THIS pass' framebuffer —
                    // an off-screen target has one per pass (§28).
                    const auto pass_fb = owner.passes.find(pending.slot);
                    const ::vsg::ref_ptr<::vsg::Framebuffer> framebuffer =
                        pass_fb == owner.passes.end() ? ::vsg::ref_ptr<::vsg::Framebuffer>() : pass_fb->second.framebuffer;
                    if (framebuffer == nullptr || framebuffer->getDevice() == nullptr) {
                        return false;
                    }
                    compileManager->add(*framebuffer, view, requirements);
                }
            }
            catch (...) {
                return false;
            }
            slot.compile_manager_generation = state.compile_manager_generation;
            ++state.compile_context_registrations;
        }

        // Compile ONLY this view: restrict the compile to the context whose
        // pre-assigned view matches, so the new/rebuild subtree is compiled
        // for the viewID it will be recorded under and no other slot is
        // touched.
        ::vsg::CompileResult result;
        try {
            ::vsg::View* const target_view = view.get();
            result = compileManager->compile(
                view, [target_view](::vsg::Context& context) { return context.view == target_view; });
        }
        catch (...) {
            return false;
        }
        if (!result) {
            return false;
        }
        // Feed dynamic data / slot / bin updates from the incremental compile
        // into the record tasks (per-frame dynamic buffers such as the DYNAMIC
        // opacity colour arrays rely on this).
        ::vsg::updateViewer(*state.viewer, result);
    }

    return true;
}

void compilePendingViews(VsgRendererState& state, const VsgDiagnostics& diagnostics)
{
    if (state.pending_compile_views.empty()) {
        return;
    }
    bool compiled = false;
    if (std::getenv("VINE_VSG_DISABLE_INCREMENTAL_COMPILE") == nullptr && state.viewer->compileManager != nullptr) {
        compiled = incrementalCompileViews(state);
    }
    if (!compiled) {
        const auto compileResult = state.viewer->compile();
        if (!compileResult) {
            diagnostics.report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::CompileFailed,
                               formatDiagnostic(u8"frame compile failed (%s): newly added content is not drawn this frame",
                                                compileResult.message.c_str()));
        }
    }
    state.pending_compile_views.clear();
}
} // namespace detail

V_VSG_NS_END
