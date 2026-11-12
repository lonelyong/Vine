#include "ContentMirror.hpp"

#include <optional>
#include <utility>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

ContentMirror::ContentMirror(const vn::graphics::DrawListSource& content, std::size_t capacity)
  : content_(&content)
  , source_(world_)
  , channel_(capacity)
{}

const vn::graphics::DrawListSource& ContentMirror::content() const noexcept
{
    return *content_;
}

vn::graphics::DrawListSource& ContentMirror::source() noexcept
{
    return source_;
}

PublishOutcome ContentMirror::publish(const Scene& scene)
{
    // A BATCH FREEZES THE VERSION ON PURPOSE, so the comparison below cannot tell "nothing moved" from "held back by
    // a batch" - and that difference is exactly what a caller acts on (T31). Asked FIRST: it is also what keeps a
    // long batch from paying a row build per frame, because nothing would be posted anyway.
    if (SceneRows::editing(scene)) {
        return PublishOutcome{ PublishStatus::InsideBatch, published_ };
    }

    const SceneRows::Version version = SceneRows::version(scene);
    if (published_.has_value() && *published_ == version) {
        // The mirror already holds this answer: a steady frame publishes nothing.
        return PublishOutcome{ PublishStatus::Unchanged, published_ };
    }
    auto rows = std::make_shared<SceneBake>();
    SceneRows::publish(scene, *rows);

    Delta delta;
    delta.kind = DeltaKind::RowsReplaced;
    if (!channel_.post(delta, std::move(rows))) {
        // Full: the version stays unpublished, so the next call retries (see the declaration). `held` still answers
        // with the publication the engine side has - it did not change.
        return PublishOutcome{ PublishStatus::Refused, published_ };
    }
    published_ = version;
    ++publish_count_;
    return PublishOutcome{ PublishStatus::Posted, published_ };
}

bool ContentMirror::setDescription(const Node& node)
{
    auto block = std::make_shared<SceneBake>();
    buildOwnFacts(node, *block);
    if (block->rows.size() != 1U) {
        return false; // not a leaf (or invisible): a payload delta describes ONE row, structure is attach/detach
    }
    Delta delta;
    delta.kind     = DeltaKind::Description;
    delta.identity = node.id();
    return channel_.post(delta, std::move(block));
}

bool ContentMirror::setState(const Node& node)
{
    Delta delta;
    delta.kind             = DeltaKind::State;
    delta.identity         = node.id();
    delta.state.opacity    = node.opacity();
    delta.state.layers     = node.layers();
    delta.state.draw_order = node.drawOrder();
    return channel_.post(delta);
}

bool ContentMirror::attach(const Node& parent, const Node& subtree, std::uint32_t child_index)
{
    auto block = std::make_shared<SceneBake>();
    buildOwnFacts(subtree, *block);
    if (block->rows.empty()) {
        return false; // an invisible subtree states no rows: "connect a hidden subtree" changes nothing
    }
    Delta delta;
    delta.kind        = DeltaKind::Attached;
    delta.identity    = parent.id();
    delta.child_index = child_index;
    return channel_.post(delta, std::move(block));
}

bool ContentMirror::detach(const Node& subtree)
{
    Delta delta;
    delta.kind     = DeltaKind::Detached;
    delta.identity = subtree.id();
    return channel_.post(delta);
}

void ContentMirror::beginFrame(std::uint64_t frame)
{
    source_.beginFrame(frame);
}

std::size_t ContentMirror::drain()
{
    channel_.drain(batch_);
    for (const OwnedDelta& owned : batch_) {
        switch (owned.delta.kind) {
            case DeltaKind::RowsReplaced:
                if (owned.rows != nullptr) {
                    world_.adopt(*owned.rows); // the world copies what it will edit; the share is released after
                }
                break;
            case DeltaKind::Attached:
                if (owned.rows != nullptr) {
                    (void)world_.attach(owned.delta.identity, owned.delta.child_index, *owned.rows);
                }
                break;
            case DeltaKind::Detached:
                (void)world_.detach(owned.delta.identity);
                break;
            case DeltaKind::Description:
                if (owned.rows != nullptr) {
                    (void)world_.setDescription(owned.delta.identity, *owned.rows);
                }
                break;
            case DeltaKind::State:
                (void)world_.setState(owned.delta.identity, owned.delta.state);
                break;
            case DeltaKind::Placement:
                (void)world_.place(owned.delta.identity, owned.delta.matrix);
                break;
            case DeltaKind::Terminate:
                // Nothing to apply: the frame that owns this mirror stops by not asking it any more (see FrameWorker).
                break;
        }
    }
    return batch_.size();
}

const RenderWorld& ContentMirror::world() const noexcept
{
    return world_;
}

std::uint64_t ContentMirror::memoHits() const noexcept
{
    return source_.memoHits();
}

std::uint64_t ContentMirror::memoStores() const noexcept
{
    return source_.memoStores();
}

std::uint64_t ContentMirror::publishCount() const noexcept
{
    return publish_count_;
}

} // namespace detail

VN_GRAPHICS_NS_END
