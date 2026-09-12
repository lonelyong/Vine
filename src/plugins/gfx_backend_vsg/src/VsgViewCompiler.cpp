#include <vine/vsg/VsgViewCompiler.hpp>

#include <cstdlib>
#include <vector>

#include <vsg/app/View.h>
#include <vsg/app/CompileManager.h>
#include <vsg/vk/Framebuffer.h>

#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgRendererState.hpp>
#include <vine/vsg/VsgUtils.hpp>

V_VSG_NS_BEGIN

namespace detail
{

bool incrementalCompileViews(VsgRendererState& state)
{
    auto compileManager = state.viewer->compileManager;
    if (compileManager == nullptr) {
        return false;
    }

    for (const auto& view : state.pending_compile_views) {
        if (view == nullptr) {
            return false;
        }

        // Locate the owning target (window target keyed by nullptr) and the
        // retained content slot the view belongs to, so the compile context
        // can carry that target's render pass (window swapchain vs off-screen
        // framebuffer — a graphics pipeline cannot be created without one).
        VsgRenderTargetEntry*     owner    = nullptr;
        ContentSlot* slot    = nullptr;
        SlotKey           slot_key;
        bool              is_window = false;
        for (auto& [target_key, target] : state.targets) {
            for (auto& [candidate_key, candidate] : target.content_slots) {
                if (candidate.ready && candidate.view == view) {
                    owner     = &target;
                    slot      = &candidate;
                    slot_key  = candidate_key;
                    is_window = (target_key == nullptr);
                    break;
                }
            }
            if (slot != nullptr) {
                break;
            }
        }
        if (slot == nullptr) {
            // A pending view that is not a content slot (e.g. a PiP or
            // program slot compiled by another path): let the caller fall
            // back to the full compile.
            return false;
        }

        // Register the slot's (render pass + view) context once. The pool's
        // pooled traversal was built by CompileManager::create(viewer, hints)
        // when the window graph was still empty, so without this the pool has
        // no context that matches this view and compile() would compile
        // nothing.
        if (!slot->compile_context_registered) {
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
                    const auto pass_fb = owner->passes.find(slot_key);
                    const ::vsg::ref_ptr<::vsg::Framebuffer> framebuffer =
                        pass_fb == owner->passes.end() ? ::vsg::ref_ptr<::vsg::Framebuffer>() : pass_fb->second.framebuffer;
                    if (framebuffer == nullptr || framebuffer->getDevice() == nullptr) {
                        return false;
                    }
                    compileManager->add(*framebuffer, view, requirements);
                }
            }
            catch (...) {
                return false;
            }
            slot->compile_context_registered = true;
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
