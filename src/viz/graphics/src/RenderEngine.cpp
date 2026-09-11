#include <vine/graphics/RenderEngine.hpp>

#include <algorithm>

#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>

V_GRAPHICS_NS_BEGIN

V_OBJECT_META_IMPL(RenderEngine, vine::Object);

RenderEngine::RenderEngine()
{
    // No implicit pipeline: the engine starts empty. The caller registers the
    // scene passes explicitly (addPass()) or through a RenderPipelineBuilder;
    // the primary interactive view (camera + content + navigation) lives in a
    // SceneView borrowing this engine.
}

RenderEngine::~RenderEngine()
{
    shutdown();
}

raw_ptr<RenderBackend> RenderEngine::backend() const
{
    return backend_.get();
}

void RenderEngine::setBackend(intrusive_ptr<RenderBackend> backend)
{
    // Re-assigning the same backend is a no-op (idempotent).
    if (backend_ == backend) {
        return;
    }
    backend_ = std::move(backend);
}

bool RenderEngine::initialize()
{
    if (backend_ == nullptr) {
        return false;
    }
    if (native_handle_ != nullptr) {
        backend_->setWindowHandle(native_handle_);
    }
    // Forward the shading preset before the backend builds its shader sets.
    backend_->setShaderPreset(shader_preset_);
    initialized_ = backend_->initialize();
    if (initialized_) {
        // Pre-frame warm-up: execute every enabled, non-clearing pass once so
        // the backend builds and compiles its content before the first frame
        // is submitted. Such passes (top / HUD content that draws over the
        // main view, e.g. the axis gizmo) carry their own scene and window
        // layer; compiling content first encountered mid-frame (i.e. inside
        // frame()) has proven unreliable in the vsg backend, and the main
        // content is pre-compiled during backend initialize. Re-execution in
        // later frames is a no-op for already-built content. Each pass is
        // opened as a pass scope and announced with its explicit order so the
        // backend can key / stack its retained per-pass state correctly even
        // though the warm-up runs non-clearing passes ahead of the clearing
        // ones.
        for (const auto& slot : slots_) {
            RenderPass* pass   = slot.pass.get();
            Scene*      content = slot.content.get();
            if (pass == nullptr || content == nullptr || !pass->enabled() || pass->clearEnabled()) {
                continue;
            }
            backend_->beginPass(pass);
            backend_->setPassOrder(slot.order);
            pass->execute(content, backend_.get());
            backend_->endPass();
        }
    }
    return initialized_;
}

void RenderEngine::shutdown()
{
    if (backend_ != nullptr && initialized_) {
        backend_->shutdown();
    }
    initialized_ = false;
}

void RenderEngine::frame(double dt)
{
    if (!initialized_ || backend_ == nullptr) {
        return;
    }
    frame_ctx_.dt = dt;
    backend_->beginFrame();

    // Fresh named-output registry per frame: every producer publishes during
    // the ordered pass run, so a consumer only ever samples this frame's
    // output and stale entries from removed producers disappear automatically.
    outputs_.clear();

    // Ordered pipeline in ascending order: negative orders run first (shadow
    // / depth / g-buffer pre-pass), the window-present pass (master camera,
    // null render target) conventionally sits at order 0, and positive orders
    // run after (post-processing / compositing, then top / HUD passes). The
    // pipeline is exactly what the caller registered - the engine auto-
    // registers nothing. Each pass resolves its declared inputs just before
    // it runs and publishes its named output right after; disabled passes are
    // skipped. The pass's explicit order is announced to the backend before
    // it runs so the backend can stack its retained content slots by that
    // order (equal orders keep registration order).
    for (const auto& slot : slots_) {
        RenderPass* pass = slot.pass.get();
        if (pass == nullptr || !pass->enabled()) {
            continue;
        }
        raw_ptr<Scene> content = slot.content.get();
        // Pass scope: the backend is told which pass is running (its identity
        // for retained per-pass GPU state, and that it is active this frame),
        // runs the whole pass, then the scope is closed so per-pass state a
        // pass did not consume cannot leak into the next pass.
        backend_->beginPass(pass);
        backend_->setPassOrder(slot.order);
        resolvePassInputs(pass);
        drawScenePass(pass, content);
        publishPassOutput(pass);
        backend_->endPass();
    }

    backend_->endFrame();
    backend_->swapBuffers();
}

const FrameContext& RenderEngine::frameContext() const
{
    return frame_ctx_;
}

bool RenderEngine::hasWindowPass(raw_ptr<Camera> camera) const
{
    for (const auto& slot : slots_) {
        RenderPass* pass = slot.pass.get();
        if (pass != nullptr && pass->enabled() && pass->camera() == camera
            && pass->renderTarget() == nullptr) {
            return true;
        }
    }
    return false;
}

void RenderEngine::addPass(intrusive_ptr<RenderPass> pass, int order)
{
    addPass(std::move(pass), nullptr, order);
}

void RenderEngine::addPass(intrusive_ptr<RenderPass> pass, intrusive_ptr<Scene> content, int order)
{
    if (pass == nullptr) {
        return;
    }
    // Registering the same pass instance twice is ignored: it would otherwise
    // run twice per frame. To rebind content use bindPassContent(); to change
    // the order remove the pass and re-add it.
    for (const auto& existing : slots_) {
        if (existing.pass.get() == pass.get()) {
            return;
        }
    }
    // Keep the slots ascending by order; equal orders preserve insertion
    // order (stable), so ties are resolved by the addPass() call sequence.
    const auto it = std::find_if(slots_.begin(), slots_.end(),
                                 [order](const Slot& slot) { return slot.order > order; });
    slots_.insert(it, Slot{ std::move(pass), std::move(content), order });
}

void RenderEngine::removePass(raw_ptr<RenderPass> pass)
{
    const auto old_size = slots_.size();
    // Capture the removed pass's order — the content-slot key of the window
    // content it drew through — before the slot is dropped.
    int order = 0;
    for (const auto& slot : slots_) {
        if (slot.pass.get() == pass) {
            order = slot.order;
            break;
        }
    }
    slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                                [pass](const Slot& slot) { return slot.pass.get() == pass; }),
                 slots_.end());

    // A pass is registered at most once, so any removal drops its only user:
    // release the backend state it retained — keyed by the pass itself (the
    // primary contract, releasePass) and by (pass camera, pass order) (the
    // legacy contract) — plus any off-screen target the pass owns.
    if (backend_ != nullptr && pass != nullptr && slots_.size() != old_size) {
        if (raw_ptr<Camera> camera = pass->camera(); camera != nullptr) {
            backend_->releaseWindowLayer(camera, order);
        }
        backend_->releasePass(pass);
        if (raw_ptr<RenderTarget> target = pass->renderTarget(); target != nullptr) {
            backend_->releaseRenderTarget(target);
        }
    }
}

void RenderEngine::clearPasses()
{
    // Snapshot every registered (pass, order) pair — the order is the
    // content-slot key of the window content each pass drew — drop all slots,
    // then release the backend resources each removed pass owned.
    std::vector<std::pair<raw_ptr<RenderPass>, int>> removed;
    removed.reserve(slots_.size());
    for (const auto& slot : slots_) {
        if (slot.pass != nullptr) {
            removed.emplace_back(slot.pass.get(), slot.order);
        }
    }

    slots_.clear();

    if (backend_ != nullptr) {
        for (const auto& entry : removed) {
            RenderPass* pass = entry.first;
            if (raw_ptr<Camera> camera = (pass != nullptr) ? pass->camera() : nullptr; camera != nullptr) {
                backend_->releaseWindowLayer(camera, entry.second);
            }
            if (pass != nullptr) {
                backend_->releasePass(pass);
            }
            if (raw_ptr<RenderTarget> target = (pass != nullptr) ? pass->renderTarget() : nullptr; target != nullptr) {
                backend_->releaseRenderTarget(target);
            }
        }
    }
}

std::size_t RenderEngine::passCount() const
{
    return slots_.size();
}

void RenderEngine::bindPassContent(raw_ptr<RenderPass> pass, intrusive_ptr<Scene> content)
{
    for (auto& slot : slots_) {
        if (slot.pass.get() == pass) {
            slot.content = std::move(content);
            return;
        }
    }
}

raw_ptr<Scene> RenderEngine::contentOf(raw_ptr<RenderPass> pass) const
{
    for (const auto& slot : slots_) {
        if (slot.pass.get() == pass) {
            return slot.content.get();
        }
    }
    return nullptr;
}

void RenderEngine::drawScenePass(raw_ptr<RenderPass> pass, raw_ptr<Scene> content)
{
    // The pass decides what a null content means: the base RenderPass draws
    // nothing (execute returns when the scene is null), while content-agnostic
    // passes such as ScreenPass still execute against their resolved inputs.
    if (pass != nullptr) {
        pass->execute(content, backend_.get());
    }
}

void RenderEngine::resolvePassInputs(raw_ptr<RenderPass> pass)
{
    if (pass == nullptr) {
        return;
    }
    const auto& names = pass->inputNames();
    if (names.empty()) {
        return;
    }
    std::vector<raw_ptr<RenderTarget>> resolved;
    resolved.reserve(names.size());
    for (const auto& name : names) {
        resolved.push_back(resolve(name));
    }
    pass->resolveInputTextures(resolved);
}

void RenderEngine::publishPassOutput(raw_ptr<RenderPass> pass)
{
    if (pass == nullptr) {
        return;
    }
    const String&    name   = pass->outputName();
    raw_ptr<RenderTarget> target = pass->renderTarget();
    if (name.empty() || target == nullptr) {
        return;
    }
    publish(name, intrusive_ptr<RenderTarget>(target));
}

void RenderEngine::publish(const String& name, intrusive_ptr<RenderTarget> target)
{
    if (name.empty() || target == nullptr) {
        return;
    }
    outputs_[name] = std::move(target);
}

raw_ptr<RenderTarget> RenderEngine::resolve(const String& name) const
{
    const auto it = outputs_.find(name);
    return (it != outputs_.end()) ? it->second.get() : nullptr;
}

void RenderEngine::unpublish(const String& name)
{
    outputs_.erase(name);
}

void RenderEngine::setShaderPreset(ShaderPreset preset)
{
    shader_preset_ = preset;
}

ShaderPreset RenderEngine::shaderPreset() const
{
    return shader_preset_;
}

void RenderEngine::resize(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    frame_ctx_.surface_width  = width;
    frame_ctx_.surface_height = height;
    if (initialized_ && backend_ != nullptr) {
        backend_->resize(width, height);
    }
    // No layout fan-out here: target sizes, pass viewports and camera
    // projections are maintained by their creators on the surface size
    // (e.g. SceneView::addSurfaceLayout). The engine only rebuilds its own
    // swapchain and records the surface size.
}

void RenderEngine::setWindowHandle(void* native_handle)
{
    native_handle_ = native_handle;
}

V_GRAPHICS_NS_END
