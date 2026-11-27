#include "PublicationStore.hpp"

#include <algorithm>

#include <vine/graphics/ContentId.hpp>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

PublicationStore::PublicationStore() = default;

PublicationStore::~PublicationStore() = default;

void PublicationStore::bindSnapshot(Scene& scene, const SceneSnapshotRef& snapshot)
{
    // THE OVERRIDE, AND ONLY AN OVERRIDE: the host says which picture this engine draws for the scene. The source is
    // still attached by the frame's own bind step (see sourceFor), so binding does not create table entries nothing
    // draws - and a bind landing while a frame is answered cannot change that frame's picture, because the frame
    // already took its publication. THE KEY IS THE SCENE'S HANDLE, because this row outlives the scene (see the class
    // note): an address key would let the next scene at that address inherit this picture.
    const SceneId                     content = scene.id();
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    for (Binding& binding : bindings_) {
        if (binding.content == content) {
            binding.snapshot = snapshot;
            return;
        }
    }
    bindings_.push_back(Binding{ content, snapshot });
}

void PublicationStore::unbindSnapshot(Scene& scene)
{
    // THE WITHDRAWAL, which is the other half of the override: "draw this one instead" ends here and frames take the
    // scene's latest publication again. A bind with an empty handle is NOT this - it binds an empty picture - which
    // is why "draw nothing" and "no override" are two verbs rather than one spelling with a footnote.
    const SceneId                     content = scene.id();
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    bindings_.erase(std::remove_if(bindings_.begin(), bindings_.end(), [content](const Binding& binding) { return binding.content == content; }),
                    bindings_.end());
}

SceneSnapshotRef PublicationStore::publicationFor(const Scene& scene)
{
    {
        const std::lock_guard<std::mutex> lock(publications_mutex_);
        if (const Binding* const binding = bindingOfLocked(scene.id())) {
            return binding->snapshot;
        }
    }
    // THE SCENE'S OWN ANSWER, read without the lock: it is the content thread's state and this call runs there.
    return scene.publishedSnapshot();
}

SnapshotSource* PublicationStore::sourceFor(Scene& scene, const SceneSnapshotRef& snapshot)
{
    if (!snapshot) {
        return nullptr;
    }
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    return sourceForLocked(scene, snapshot);
}

void PublicationStore::beginFrame(std::uint64_t frame)
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    frame_ = frame;
    // DROP WHAT NO FRAME NAMES ANY MORE: a source is named by the BUILD of a frame (which marks frame + 1, because this
    // token is the one the previous frame opened), so anything last used before the previous frame is a picture no
    // frame draws. With one frame in flight that keeps exactly "the one being answered + the one being built".
    publications_.erase(
        std::remove_if(publications_.begin(),
                       publications_.end(),
                       [frame](const Publication& publication) { return publication.last_used_frame + 1U < frame; }),
        publications_.end());
    for (const Publication& publication : publications_) {
        publication.source->beginFrame(frame);
    }
    // AN OVERRIDE ENDS WITH ITS SCENE, and the scene cannot say so: content never learns which engines bound it, so
    // the store asks the identity instead - the handle the scene held until its destructor released it. Dropping the
    // row releases the publication it kept, which is the memory a destroyed scene's override would otherwise hold
    // for the life of the engine.
    bindings_.erase(
        std::remove_if(bindings_.begin(), bindings_.end(), [](const Binding& binding) { return !ContentRegistry::instance().alive(binding.content.handle()); }),
        bindings_.end());
}

std::size_t PublicationStore::publicationCount() const noexcept
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    return publications_.size();
}

std::size_t PublicationStore::bindingCount() const noexcept
{
    const std::lock_guard<std::mutex> lock(publications_mutex_);
    return bindings_.size();
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

SnapshotSource* PublicationStore::sourceForLocked(Scene& scene, const SceneSnapshotRef& snapshot)
{
    if (Publication* const existing = publicationOfLocked(scene.id(), snapshot.revision())) {
        existing->last_used_frame = frame_ + 1; // the frame being built right now
        return existing->source.get();
    }
    // One source per publication: the handshake is the first frame that draws this value. The vector is reserved for
    // the small number of publications a frame draws (a main scene, a HUD, an overlay, and the previous frame's).
    publications_.reserve(8U);
    publications_.push_back(Publication{ scene.id(), snapshot.revision(), std::make_unique<SnapshotSource>(), frame_ + 1 });
    SnapshotSource* const source = publications_.back().source.get();
    source->setSnapshot(snapshot.published());
    // A source born IN a frame joins that frame: the token was opened before it existed, so without this its first
    // frame would collect once per pass - the one frame where a new content costs the most. Opening the token here is
    // idempotent with the next beginFrame and never rewinds the memo (see DrawListMemo::beginFrame).
    source->beginFrame(frame_);
    return source;
}

PublicationStore::Publication* PublicationStore::publicationOfLocked(SceneId content, std::uint64_t revision) noexcept
{
    for (Publication& publication : publications_) {
        if (publication.content == content && publication.revision == revision) {
            return &publication;
        }
    }
    return nullptr;
}

const PublicationStore::Binding* PublicationStore::bindingOfLocked(SceneId content) const noexcept
{
    for (const Binding& binding : bindings_) {
        if (binding.content == content) {
            return &binding;
        }
    }
    return nullptr;
}

} // namespace detail

VN_GRAPHICS_NS_END
