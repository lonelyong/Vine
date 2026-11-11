#include "FrameWorker.hpp"

#include "FrameExpand.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

FrameWorker::FrameWorker() = default;

FrameWorker::~FrameWorker() = default;

bool FrameWorker::publish(const Scene& scene)
{
    ContentMirror* mirror = mirrorOf(scene);
    if (mirror == nullptr) {
        // The handshake: one mirror per content, created the first time the content is published. The vector is
        // reserved for the small number of contents a frame draws (a main scene, a HUD, an overlay) so a steady frame
        // never reallocates it.
        mirrors_.reserve(4U);
        mirrors_.push_back(std::make_unique<ContentMirror>(scene));
        mirror = mirrors_.back().get();
    }
    return mirror->publish(scene);
}

std::size_t FrameWorker::run(FrameDescription& frame)
{
    // 1. DRAIN: every mirror applies what its content published, in post order, before anything reads the rows.
    std::size_t drained = 0U;
    for (const std::unique_ptr<ContentMirror>& mirror : mirrors_) {
        drained += mirror->drain();
    }
    applied_ += drained;

    // 2. BIND: a draw that names a CONTENT is re-pointed at that content's mirror, so the expansion below stops asking
    //    the content (which is the whole point of the mirror, and the only thing that makes the expand safe to run on
    //    another thread). A source this worker does not mirror is left alone and counted: correct on the calling
    //    thread, a cross-thread read anywhere else - which is exactly what the count is for.
    for (FrameDescription::Pass& pass : frame.passes) {
        if (!pass.draw.has_value()) {
            continue;
        }
        for (DrawDescription& draw : pass.draw->draws) {
            if (draw.content == nullptr) {
                continue;
            }
            ContentMirror* mirror = mirrorOf(*draw.content);
            if (mirror == nullptr) {
                ++unbound_;
                continue;
            }
            draw.content = &mirror->source();
        }
    }

    // 3. EXPAND: the frame's one O(drawables) step, against the mirrors the bind just selected.
    expandFrame(frame);
    return drained;
}

std::size_t FrameWorker::mirrorCount() const noexcept
{
    return mirrors_.size();
}

std::uint64_t FrameWorker::applied() const noexcept
{
    return applied_;
}

std::uint64_t FrameWorker::unbound() const noexcept
{
    return unbound_;
}

ContentMirror* FrameWorker::mirrorOf(const vn::graphics::DrawListSource& source) noexcept
{
    for (const std::unique_ptr<ContentMirror>& mirror : mirrors_) {
        // Either spelling means "this content": the CONTENT itself (a frame described on the content's thread) or the
        // mirror (a description that has already been bound once). Matching both is what makes a second `run` on the
        // same frame a no-op here instead of a false "unbound".
        if (&mirror->content() == &source || &mirror->source() == &source) {
            return mirror.get();
        }
    }
    return nullptr;
}

} // namespace detail

VN_GRAPHICS_NS_END
