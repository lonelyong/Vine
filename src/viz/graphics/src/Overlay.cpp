#include <vine/graphics/Overlay.hpp>

#include <vine/graphics/CameraMirror.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/Scene.hpp>

V_GRAPHICS_NS_BEGIN

V_OBJECT_META_IMPL(Overlay, vine::Object);

namespace
{

/**
 * @brief Translates this class' nested mirror mode into the shared one.
 *
 * Overlay predates CameraMirror.hpp and keeps its nested enum so existing callers compile, but the
 * MECHANISM is the shared one (applyCameraMirror) — so the camera math exists once, and this switch
 * is the only place the two spellings have to agree.
 *
 * @param mode Overlay mirror mode to translate.
 * @return The equivalent shared mirror mode.
 */
MirrorMode sharedMirrorMode(Overlay::MirrorMode mode) noexcept
{
    switch (mode) {
    case Overlay::MirrorMode::None: return MirrorMode::None;
    case Overlay::MirrorMode::Orientation: return MirrorMode::Orientation;
    case Overlay::MirrorMode::FullView: return MirrorMode::FullView;
    }
    return MirrorMode::None;
}

}  // namespace

Overlay::Overlay()
  : pass_(new RenderPass())
{
    // Overlays draw over the previous frame: do not clear the whole surface
    // and never depth-occlude (always on top).
    pass_->setClearEnabled(false);
    pass_->setOcclusionEnabled(false);
}

Overlay::~Overlay() = default;

raw_ptr<RenderPass> Overlay::pass() const
{
    return pass_.get();
}

void Overlay::setPass(intrusive_ptr<RenderPass> pass)
{
    pass_ = std::move(pass);
}

raw_ptr<Scene> Overlay::content() const
{
    return content_.get();
}

void Overlay::setContent(intrusive_ptr<Scene> content)
{
    content_ = std::move(content);
}

int Overlay::zOrder() const
{
    return z_order_;
}

void Overlay::setZOrder(int order)
{
    z_order_ = order;
}

bool Overlay::visible() const
{
    return visible_;
}

void Overlay::setVisible(bool visible)
{
    visible_ = visible;
}

void Overlay::setMirrorMode(MirrorMode mode)
{
    mirror_mode_ = mode;
    applyMirror();
}

Overlay::MirrorMode Overlay::mirrorMode() const
{
    return mirror_mode_;
}

void Overlay::setSourceCamera(raw_ptr<Camera> camera)
{
    source_camera_ = camera;
    applyMirror();
}

void Overlay::update(double dt)
{
    (void)dt;
    applyMirror();
}

void Overlay::applyMirror()
{
    // The mirror math belongs to CameraMirror (the same rule drives the HUD passes): this class only
    // owns the source camera and the mode. Null pointers and MirrorMode::None are handled there.
    applyCameraMirror(pass_ != nullptr ? pass_->camera() : nullptr, source_camera_,
                      sharedMirrorMode(mirror_mode_));
}

V_GRAPHICS_NS_END
