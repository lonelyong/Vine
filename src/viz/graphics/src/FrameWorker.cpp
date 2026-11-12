#include "FrameWorker.hpp"

#include "FrameExpand.hpp"
#include "SceneRows.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

FrameWorker::FrameWorker() = default;

FrameWorker::~FrameWorker() = default;

ScenePublishResult FrameWorker::publish(Scene& scene)
{
    SnapshotSource* source = nullptr;
    {
        // The list is looked up and grown under its lock, and the lock is RELEASED before anything is published: what
        // the two halves share is the list, not the source, and a content's publish is O(rows).
        const std::lock_guard<std::mutex> lock(publications_mutex_);
        source = sourceOfLocked(scene);
        if (source == nullptr) {
            // The handshake: one source per content, created the first time the content is published. The vector is
            // reserved for the small number of contents a frame draws (a main scene, a HUD, an overlay) so a steady
            // frame never reallocates it.
            publications_.reserve(4U);
            publications_.push_back(Publication{ &scene, std::make_unique<SnapshotSource>() });
            source = publications_.back().source.get();
            // A source born IN a frame joins that frame: the token was opened before it existed, so without this its
            // first frame would collect once per pass - the one frame where a new content costs the most. Opening the
            // token here is idempotent with the next beginFrame and never rewinds the memo (see
            // DrawListMemo::beginFrame).
            source->beginFrame(frame_);
        }
    }

    // THE ONE PUBLICATION ENTRY IS THE SCENE'S. A batch answer leaves the source on the previous snapshot; Posted and
    // Unchanged both hand over the current one (the same share when nothing moved).
    const ScenePublishResult result = scene.publish();
    if (result.status != ScenePublishStatus::InsideBatch) {
        source->setSnapshot(SceneRows::snapshot(scene));
    }
    return result;
}

void FrameWorker::beginFrame(std::uint64_t frame)
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    frame_ = frame;
    for (const Publication& publication : publications_) {
        publication.source->beginFrame(frame);
    }
}

void FrameWorker::runPass(PassDraw& pass, FrameMode mode)
{
    if (mode == FrameMode::Published) {
        // BIND: a draw that names a CONTENT is re-pointed at that content's PUBLISHED SNAPSHOT, so the expansion below
        // stops asking the content - which is what makes the expand safe off the content's thread. A source this worker
        // does not publish is left alone and counted: correct on the calling thread, a cross-thread read anywhere else,
        // which is exactly what the count is for.
        for (DrawDescription& draw : pass.draws) {
            if (draw.content == nullptr) {
                continue;
            }
            SnapshotSource* source = sourceOf(*draw.content);
            if (source == nullptr) {
                ++unbound_;
                continue;
            }
            draw.content = source;
        }
    }
    // The INLINE mode binds nothing: the caller IS the content's thread, so asking the content is exactly right, and
    // re-pointing the draw would spend a snapshot's copy to reproduce the answer the content gives for free.
    expandPass(pass);
}

void FrameWorker::run(FrameDescription& frame, FrameMode mode)
{
    for (FrameDescription::Pass& entry : frame.passes) {
        if (entry.draw.has_value()) {
            runPass(*entry.draw, mode);
        }
    }
}

std::size_t FrameWorker::publicationCount() const noexcept
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    return publications_.size();
}

std::uint64_t FrameWorker::unbound() const noexcept
{
    return unbound_;
}

std::uint64_t FrameWorker::memoHits() const noexcept
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    std::uint64_t hits = 0U;
    for (const Publication& publication : publications_) {
        hits += publication.source->memoHits();
    }
    return hits;
}

std::uint64_t FrameWorker::memoStores() const noexcept
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    std::uint64_t stores = 0U;
    for (const Publication& publication : publications_) {
        stores += publication.source->memoStores();
    }
    return stores;
}

SnapshotSource* FrameWorker::sourceOf(const vn::graphics::DrawListSource& source) noexcept
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    return sourceOfLocked(source);
}

SnapshotSource* FrameWorker::sourceOfLocked(const vn::graphics::DrawListSource& source) noexcept
{
    for (const Publication& publication : publications_) {
        // Either spelling means "this content": the CONTENT itself (a frame described on the content's thread) or the
        // snapshot source it was bound to. Matching both is what makes a second `run` on the same frame a no-op here
        // instead of a false "unbound".
        if (publication.content == &source || publication.source.get() == &source) {
            return publication.source.get();
        }
    }
    return nullptr;
}

} // namespace detail

VN_GRAPHICS_NS_END
