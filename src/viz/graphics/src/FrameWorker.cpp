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
        // A mirror born IN a frame joins that frame: the token was opened before it existed, so without this its first
        // frame would collect once per pass - the one frame where a new content costs the most. Opening the token here
        // is idempotent with the next beginFrame and never rewinds the memo (see DrawListMemo::beginFrame).
        mirror->beginFrame(frame_);
    }
    return mirror->publish(scene);
}

void FrameWorker::beginFrame(std::uint64_t frame)
{
    frame_ = frame;
    for (const std::unique_ptr<ContentMirror>& mirror : mirrors_) {
        mirror->beginFrame(frame);
    }
}

std::size_t FrameWorker::drain()
{
    // Every mirror applies what its content published, in post order, before anything reads the rows.
    std::size_t drained = 0U;
    for (const std::unique_ptr<ContentMirror>& mirror : mirrors_) {
        drained += mirror->drain();
    }
    applied_ += drained;
    return drained;
}

void FrameWorker::runPass(PassDraw& pass, FrameMode mode)
{
    if (mode == FrameMode::Mirrored) {
        // BIND: a draw that names a CONTENT is re-pointed at that content's mirror, so the expansion below stops
        // asking the content - which is what makes the expand safe off the content's thread. A source this worker does
        // not mirror is left alone and counted: correct on the calling thread, a cross-thread read anywhere else,
        // which is exactly what the count is for.
        for (DrawDescription& draw : pass.draws) {
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
    // The INLINE mode binds nothing: the caller IS the content's thread, so asking the content is exactly right, and
    // re-pointing the draw would spend a mirror's copy to reproduce the answer the content gives for free.
    expandPass(pass);
}

std::size_t FrameWorker::run(FrameDescription& frame, FrameMode mode)
{
    const std::size_t drained = drain();
    for (FrameDescription::Pass& entry : frame.passes) {
        if (entry.draw.has_value()) {
            runPass(*entry.draw, mode);
        }
    }
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

std::uint64_t FrameWorker::memoHits() const noexcept
{
    std::uint64_t hits = 0U;
    for (const std::unique_ptr<ContentMirror>& mirror : mirrors_) {
        hits += mirror->memoHits();
    }
    return hits;
}

std::uint64_t FrameWorker::memoStores() const noexcept
{
    std::uint64_t stores = 0U;
    for (const std::unique_ptr<ContentMirror>& mirror : mirrors_) {
        stores += mirror->memoStores();
    }
    return stores;
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
