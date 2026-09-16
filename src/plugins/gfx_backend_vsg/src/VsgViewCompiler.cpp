#include <vine/vsg/VsgViewCompiler.hpp>

#include <cstddef>
#include <cstdlib>
#include <utility>
#include <vector>

#include <vsg/app/CompileTraversal.h>
#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/vk/Context.h>
#include <vsg/vk/Framebuffer.h>

#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgRendererState.hpp>
#include <vine/vsg/VsgUtils.hpp>

V_VSG_NS_BEGIN

namespace detail
{

VsgCompileManager::VsgCompileManager(::vsg::Viewer& viewer, ::vsg::ref_ptr<::vsg::ResourceHints> hints) :
    ::vsg::Inherit<::vsg::CompileManager, VsgCompileManager>(viewer, std::move(hints))
{
    // The base constructor gave the pool a traversal built from the live command graph, whose contexts
    // are its own derivation. This backend registers every context it needs (see the class note), so
    // the pool starts over with one traversal that has none -- and the contexts that walk created are
    // released with it.
    compileTraversals    = CompileTraversals::create(viewer.status);
    numCompileTraversals = 1;
    compileTraversals->add(::vsg::CompileTraversal::create());
}

std::size_t VsgCompileManager::forget(const ::vsg::View* view)
{
    if (view == nullptr) {
        return 0;
    }
    // The pool hands its traversal out for the duration of a compile and every compile gives it back
    // (CompileManager::compile does, whether the compile succeeded or threw), so this waits only while
    // a compile is actually running -- which the teardown that calls it cannot be doing.
    std::size_t forgotten  = 0;
    auto        traversals = takeCompileTraversals(numCompileTraversals);
    for (auto& traversal : traversals) {
        forgotten += traversal->contexts.remove_if(
            [view](const ::vsg::ref_ptr<::vsg::Context>& context) { return context->view.get() == view; });
        compileTraversals->add(traversal);
    }
    return forgotten;
}

void forgetCompileContext(VsgRendererState& state, const ::vsg::View* view)
{
    if (view == nullptr || state.viewer == nullptr) {
        return;
    }
    // The session's manager is this backend's own -- initialize() installs it before the viewer could
    // create one lazily -- so the cast holds; a manager that is not ours holds no registration of ours.
    if (auto manager = state.viewer->compileManager.cast<VsgCompileManager>()) {
        const std::size_t forgotten = manager->forget(view);
        state.compile_context_registrations -= std::min(forgotten, state.compile_context_registrations);
    }
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

        // Register the slot's (render pass + view) context once. The pool's
        // pooled traversal was built by CompileManager::create(viewer, hints)
        // when the window graph was still empty, so without this the pool has
        // no context that matches this view and compile() would compile
        // nothing -- the manager this backend installs starts with no contexts at all
        // (VsgCompileManager). The registration is released where the slot dies
        // (forgetCompileContext), so the pool holds one per LIVE slot.
        if (!slot.compile_context_registered) {
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
            slot.compile_context_registered = true;
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
