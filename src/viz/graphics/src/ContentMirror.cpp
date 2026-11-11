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

bool ContentMirror::publish(const Scene& scene)
{
    const SceneRows::Version version = SceneRows::version(scene);
    if (published_.has_value() && *published_ == version) {
        return false; // the mirror already holds this answer: a steady frame publishes nothing
    }
    auto rows = std::make_shared<SceneBake>();
    SceneRows::publish(scene, *rows);

    Delta delta;
    delta.kind = DeltaKind::RowsReplaced;
    if (!channel_.post(delta, std::move(rows))) {
        return false; // full: the version stays unpublished, so the next call retries (see the declaration)
    }
    published_ = version;
    ++publish_count_;
    return true;
}

bool ContentMirror::setState(const Node& node)
{
    Delta delta;
    delta.kind             = DeltaKind::State;
    delta.identity         = &node;
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
    delta.identity    = &parent;
    delta.child_index = child_index;
    return channel_.post(delta, std::move(block));
}

bool ContentMirror::detach(const Node& subtree)
{
    Delta delta;
    delta.kind     = DeltaKind::Detached;
    delta.identity = &subtree;
    return channel_.post(delta);
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

std::uint64_t ContentMirror::publishCount() const noexcept
{
    return publish_count_;
}

} // namespace detail

VN_GRAPHICS_NS_END
