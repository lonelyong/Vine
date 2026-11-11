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
