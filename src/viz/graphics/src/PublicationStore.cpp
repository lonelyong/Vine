#include "PublicationStore.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

PublicationStore::PublicationStore() = default;

PublicationStore::~PublicationStore() = default;

ScenePublishResult PublicationStore::publish(Scene& scene)
{
    // THE CONTENT'S SOURCE EXISTS BEFORE ANYTHING IS HANDED OVER: a draw's bind step looks it up, and a scene that
    // answers InsideBatch still has to be findable - that frame draws the picture the batch started from.
    SnapshotSource* const source = sourceFor(scene);

    // A HOST THAT BOUND THIS SCENE OWNS ITS PUBLICATION (see bindSnapshot). Replacing the picture it was handed at
    // frame start would make that call pointless, so the answer is the publication the source already holds and the
    // status says the frame published nothing.
    if (hasExplicitSnapshot(scene)) {
        return ScenePublishResult{ ScenePublishStatus::Unchanged, SceneSnapshotRef{ source->snapshot() } };
    }

    // THE ONE PUBLICATION ENTRY IS THE SCENE'S, and what it produced travels back as a VALUE - so this is the only
    // place that decides what the frame draws, and the answer already says which publication it is.
    const ScenePublishResult result = scene.publish();
    if (result.status != ScenePublishStatus::InsideBatch) {
        source->setSnapshot(result.snapshot.published());
    }
    return result;
}

void PublicationStore::bindSnapshot(Scene& scene, const SceneSnapshotRef& snapshot)
{
    // The EXPLICIT handoff (see RenderEngine::bindSnapshot): the host published, the host says which picture to draw,
    // and from here on the frame does not publish this scene itself (see publish). An empty handle is a legitimate
    // bound value - it means "draw nothing", which is the same answer as a scene whose content draws nothing.
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    Publication*                      publication = publicationOfLocked(scene);
    if (publication == nullptr) {
        (void)sourceForLocked(scene); // attaches the record this scene's draws will be bound through
        publication = publicationOfLocked(scene);
    }
    publication->explicit_snapshot = true;
    publication->source->setSnapshot(snapshot.published());
}

bool PublicationStore::hasExplicitSnapshot(const Scene& scene)
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    const Publication* const          publication = publicationOfLocked(scene);
    return publication != nullptr && publication->explicit_snapshot;
}

void PublicationStore::beginFrame(std::uint64_t frame)
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    frame_ = frame;
    for (const Publication& publication : publications_) {
        publication.source->beginFrame(frame);
    }
}

std::size_t PublicationStore::publicationCount() const noexcept
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    return publications_.size();
}

std::uint64_t PublicationStore::memoHits() const noexcept
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    std::uint64_t hits = 0U;
    for (const Publication& publication : publications_) {
        hits += publication.source->memoHits();
    }
    return hits;
}

std::uint64_t PublicationStore::memoStores() const noexcept
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    std::uint64_t stores = 0U;
    for (const Publication& publication : publications_) {
        stores += publication.source->memoStores();
    }
    return stores;
}

SnapshotSource* PublicationStore::sourceFor(Scene& scene)
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    return sourceForLocked(scene);
}

SnapshotSource* PublicationStore::sourceForLocked(Scene& scene)
{
    if (SnapshotSource* existing = sourceOfLocked(scene)) {
        return existing;
    }
    // The handshake: one source per content, created the first time the content is published or bound. The vector is
    // reserved for the small number of contents a frame draws (a main scene, a HUD, an overlay) so a steady frame
    // never reallocates it.
    publications_.reserve(4U);
    publications_.push_back(Publication{ &scene, std::make_unique<SnapshotSource>(), false });
    SnapshotSource* const source = publications_.back().source.get();
    // A source born IN a frame joins that frame: the token was opened before it existed, so without this its first
    // frame would collect once per pass - the one frame where a new content costs the most. Opening the token here is
    // idempotent with the next beginFrame and never rewinds the memo (see DrawListMemo::beginFrame).
    source->beginFrame(frame_);
    return source;
}

PublicationStore::Publication* PublicationStore::publicationOfLocked(const Scene& scene) noexcept
{
    for (Publication& publication : publications_) {
        if (publication.content == &scene) {
            return &publication;
        }
    }
    return nullptr;
}

SnapshotSource* PublicationStore::sourceOf(const vn::graphics::DrawListSource& source) noexcept
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    return sourceOfLocked(source);
}

SnapshotSource* PublicationStore::sourceOfLocked(const vn::graphics::DrawListSource& source) noexcept
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
