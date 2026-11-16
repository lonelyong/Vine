#include "ScenePublisher.hpp"

#include "DrawListBake.hpp"
#include "DrawListMemo.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include <vine/graphics/Group.hpp>

VN_GRAPHICS_NS_BEGIN

using vn::math::Vec3d;

namespace detail
{

[[nodiscard]] std::shared_ptr<const std::vector<vn::graphics::RenderCommand>> noCommands()
{
    static const auto kNoCommands = std::make_shared<const std::vector<vn::graphics::RenderCommand>>();
    return kNoCommands;
}

namespace
{
/// @brief Bumps every bounds stamp in @p node's subtree (a scene-level invalidation).
///
/// One call per scene edit is rare enough that the O(nodes x depth) propagation invalidateBounds()
/// performs per node is not worth a second bump path.
void invalidateBoundsUnder(Node& node)
{
    node.invalidateBounds();
    if (const auto* group = obj_cast<Group>(&node)) {
        for (const auto& child : group->children()) {
            invalidateBoundsUnder(*child);
        }
    }
}

} // namespace

ScenePublisher::ScenePublisher(vn::graphics::Scene& scene) noexcept
  : scene_(scene)
{}

ScenePublisher::~ScenePublisher() = default;

std::shared_ptr<const std::vector<vn::graphics::RenderCommand>>
ScenePublisher::collectRenderCommands(const vn::graphics::View& view) const
{
    // THE EMPTY ANSWER, SHARED rather than built: an empty scene asks for nothing, and paying an allocation for
    // "nothing" would make the cheap case the expensive one. A root that is no longer a ROOT is the same answer,
    // and the check runs BEFORE any cache lookup on purpose: a list built while the node was still this scene's
    // root must not answer for a scene that no longer draws it (see Scene::setRoot).
    if (!scene_.visible_ || !scene_.hasRootedRoot()) {
        return noCommands();
    }

    // ONE SELECTION RULE. The content query answers from the SAME immutable publication a frame draws: the snapshot
    // is brought up to date (rows, boxes and the values a row carries) and `selectSnapshot` is THE view half - the
    // function the frame's source also runs. The direct walk (`selectFromBake` over the live rows) is gone, so a
    // rule change is one edit, not two (G1 / G5).
    //
    // INSIDE AN EDIT BATCH the answer is the picture the batch started from, exactly as `publish()` reports it: one
    // publication per batch is the contract a frame rests on, and the content query says the same thing.
    const SceneSnapshotPtr current = snapshot();

    // The memo is the same `DrawListMemo` the frame's source uses, keyed by the publication's revision: a new
    // snapshot (an edit, a moved box, a value that moved) is a new key, so invalidation is the snapshot's own
    // rule rather than a second epoch to compare.
    const Mat4d view_proj = view.viewProjection();
    const bool  memoising = content_frame_ != 0; // nothing is memoised until a frame is open (a caller drives it)
    if (memoising && content_memo_ == nullptr) {
        content_memo_ = std::make_unique<DrawListMemo>();
    }
    if (memoising) {
        if (DrawListMemo::Entry* hit = content_memo_->find(view_proj, view.camera.eye, view.viewport, view.layer_mask, current->revision)) {
            ++content_reuse_count_;
            hit->last_used_frame = content_frame_;
            return hit->commands;
        }
    }
    ++content_collect_count_;

    std::vector<vn::graphics::RenderCommand> commands;
    bool                                    viewport_used = false;
    selectSnapshot(*current, view, commands, viewport_used);
    // Into the shared list by MOVE: the selection's result is the only copy that ever exists.
    auto shared = std::make_shared<const std::vector<vn::graphics::RenderCommand>>(std::move(commands));
    if (memoising) {
        if (DrawListMemo::Entry* entry = content_memo_->find(view_proj, view.camera.eye, view.viewport, view.layer_mask, current->revision)) {
            entry->commands           = shared;
            entry->viewport_sensitive = viewport_used;
            entry->last_used_frame    = content_frame_;
        }
        else {
            (void)content_memo_->store(view_proj, view.camera.eye, view.viewport, view.layer_mask, viewport_used, current->revision, shared, content_frame_);
        }
    }
    return shared;
}

vn::graphics::ScenePublishResult ScenePublisher::publish()
{    // A BATCH HOLDS THE PUBLICATION BACK (see Scene::beginEdit): the previous snapshot stays the answer, so a frame that
    // lands mid-batch draws the state the batch started from - and the answer says which picture that is.
    if (edit_depth_ > 0) {
        return vn::graphics::ScenePublishResult{ vn::graphics::ScenePublishStatus::InsideBatch, published_snapshot_ };
    }

    // `snapshot` is the one builder: it brings the rows up to date, shares unchanged chunks with the previous
    // publication, and returns the cached value when nothing moved. Its revision is the publication id, so the
    // question "is this a new publication" is one comparison on the handle the caller already holds.
    const SceneSnapshotPtr published = snapshot();
    if (!published_snapshot_.valid() || published_snapshot_.revision() != published->revision) {
        published_snapshot_ = vn::graphics::SceneSnapshotRef{ published };
        return vn::graphics::ScenePublishResult{ vn::graphics::ScenePublishStatus::Posted, published_snapshot_ };
    }
    return vn::graphics::ScenePublishResult{ vn::graphics::ScenePublishStatus::Unchanged, published_snapshot_ };
}

const vn::graphics::SceneSnapshotRef& ScenePublisher::publishedSnapshot() const noexcept
{
    return published_snapshot_;
}

void ScenePublisher::beginEdit()
{
    // A COUNTER, not a flag: a helper that opens its own batch must compose with the caller's (see Scene::beginEdit).
    ++edit_depth_;
}

void ScenePublisher::endEdit()
{
    if (edit_depth_ > 0) {
        --edit_depth_;
    }
    // The version is NOT forced up here: nothing has been collected yet, so `bake_epoch_` may be behind the graph. The
    // next collection catches the pair up (see ensureRows) - and that collection is the one a frame's publication
    // reads, which is exactly where the whole batch has to be visible.
}

void ScenePublisher::setContentFrame(std::uint64_t frame)
{
    if (content_frame_ == frame) {
        return; // idempotent: every pass of a frame may announce the same token
    }
    const std::uint64_t previous = content_frame_;
    content_frame_               = frame;
    if (content_memo_ == nullptr) {
        return;
    }
    // ONE FRAME OF GRACE, then the entry goes (see DrawListMemo::beginFrame): this is what replaced "clear everything at
    // every frame boundary" - a list is kept while some view keeps asking for it.
    content_memo_->beginFrame(previous);
}

void ScenePublisher::invalidateContent()
{
    if (scene_.root_ != nullptr) {
        // Scene-level facts (a swapped root, scene visibility / opacity, an edit no setter could see) are announced
        // on the root's own subtree epoch: the memo then drops its entries at the next collect, and the per-entry
        // revision below is still the key that says which scene version a list was built for.
        scene_.root_->announceContentChange();
        // The bounds caches are per node and keyed by ANNOUNCEMENTS, and the built-in setters already announce - so
        // what this reaches is the edit they could not see: data written through a buffer, a custom node's own state.
        // Bumping every stamp makes the next collect recompute every box once (and re-cache), which is what "picked
        // up at once" has to mean for the culling cache too.
        invalidateBoundsUnder(*scene_.root_);
    }
}

SceneSnapshotPtr ScenePublisher::snapshot() const
{
    // Ensuring the rows is what makes the answer CURRENT: the build is keyed by the epoch, and the refresh is what
    // notices a box or material value that moved under it. Inside an edit batch the PUBLISHED pair stays where it
    // was, so this returns the previous cached snapshot (or builds the pre-batch one on first use).
    (void)ensureRows();
    const std::uint64_t epoch  = published_epoch_;
    const std::uint64_t boxes  = published_boxes_;
    const std::uint64_t values = published_values_;

    SceneBake* rows = content_bake_.get();
    if (rows == nullptr) {
        return makeSceneSnapshot(SceneBake{}, epoch, boxes, values, nullptr);
    }
    const SceneSnapshotPtr& cached = rows->snapshot_cache;
    if (cached != nullptr && cached->epoch == epoch && cached->boxes == boxes && cached->values == values) {
        return cached; // nothing moved: the previous publication is still the answer
    }

    SceneSnapshotPtr published = makeSceneSnapshot(*rows, epoch, boxes, values, cached);
    rows->snapshot_cache       = published;
    return published;
}

const SceneBake& ScenePublisher::ensureRows() const
{
    // A scene with no rooted root DRAWS NOTHING (see collectRenderCommands), so its rows are empty - and that is a
    // state, not an error: a publication of such a scene is an empty table and the frame stays correct. This is the
    // total contract of this function; a caller (the publication) has no business repeating the guard.
    if (!scene_.hasRootedRoot()) {
        if (content_bake_ == nullptr) {
            content_bake_ = std::make_unique<SceneBake>();
        }
        content_bake_->rows.clear();
        content_bake_->nodes.clear();
        content_bake_->node_epochs.clear();
        content_bake_->snapshot_cache.reset();
        return *content_bake_;
    }

    // THE GRAPH HALF, ONCE PER EPOCH: every node's own facts, then the one fold over them (see DrawListBake.hpp).
    // A moved camera then pays the VIEW half only - the layer prune, the frustum test and the sort - instead of
    // recomputing every matrix and every fold, which is what a collection used to cost per view
    // (measured: SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera). It is a named step because a collection
    // is not the only reader of the rows any more: the publication is built from them (see snapshot).
    const std::uint64_t epoch               = scene_.root_->contentEpoch();
    bool                refreshed_in_place = false;
    if (content_bake_ == nullptr || bake_epoch_ != epoch) {
        // THE INCREMENTAL PATH. A node edit bumps its own epoch and every ancestor's, so the rows whose epochs moved
        // are the edited rows plus their ancestors. When the structure did not change and every edited row's own
        // facts are still "hot" (placement / opacity / layers / order / view placement), re-read those rows and
        // re-fold only their topmost subtrees. Anything else - a child insert / remove, a state or geometry change,
        // a material whose values moved - falls back to the full build; the fast path must never guess.
        if (content_bake_ != nullptr && !content_bake_->rows.empty() &&
            content_bake_->rows[0].identity == scene_.root_->id() &&
            content_bake_->nodes.size() == content_bake_->rows.size() &&
            content_bake_->node_epochs.size() == content_bake_->rows.size()) {
            std::vector<std::uint32_t> changed;
            bool                       structure_ok = true;
            for (std::uint32_t index = 0; index < content_bake_->rows.size(); ++index) {
                const Row&  row  = content_bake_->rows[index];
                const Node* node = content_bake_->nodes[index];
                if (node == nullptr || node->id() != row.identity || !node->isVisible()) {
                    structure_ok = false;
                    break;
                }
                const Node* expected_parent =
                    row.parent == SceneBake::kNoParent ? nullptr : content_bake_->nodes[row.parent];
                if (node->parent() != expected_parent) {
                    structure_ok = false;
                    break;
                }
                if (row.child_count != 0U) {
                    const auto* group = obj_cast<Group>(node);
                    if (group == nullptr || group->children().size() != row.child_count) {
                        structure_ok = false;
                        break;
                    }
                    // The row table's walk order IS the child order: checking the identities here catches a reorder or
                    // a same-count child swap, which no child count can see (and which the hot path must not accept).
                    std::uint32_t cursor = index + 1U;
                    for (const auto& child : group->children()) {
                        if (!child->isVisible()) {
                            continue; // an invisible child states no row (see bakeSubtree)
                        }
                        if (cursor >= row.end || content_bake_->nodes[cursor] != child.get()) {
                            structure_ok = false;
                            break;
                        }
                        cursor = content_bake_->rows[cursor].end;
                    }
                    if (!structure_ok || cursor != row.end) {
                        structure_ok = false;
                        break;
                    }
                }
                if (node->contentEpoch() != content_bake_->node_epochs[index]) {
                    changed.push_back(index);
                }
            }

            if (structure_ok && !changed.empty()) {
                std::vector<std::uint32_t> hot;
                bool                       compatible = true;
                for (const std::uint32_t index : changed) {
                    const RowRefresh answer = refreshRowFacts(*content_bake_->nodes[index], *content_bake_, index);
                    if (answer == RowRefresh::Incompatible) {
                        compatible = false;
                        break;
                    }
                    if (answer == RowRefresh::HotChange) {
                        hot.push_back(index);
                    }
                }
                if (compatible && !hot.empty()) {
                    for (const std::uint32_t index : hot) {
                        bool has_hot_ancestor = false;
                        for (std::uint32_t above = content_bake_->rows[index].parent;
                             above != SceneBake::kNoParent;
                             above = content_bake_->rows[above].parent) {
                            if (std::find(hot.begin(), hot.end(), above) != hot.end()) {
                                has_hot_ancestor = true;
                                break;
                            }
                        }
                        if (!has_hot_ancestor) {
                            (void)resolveSubtree(*content_bake_, index);
                        }
                    }
                    for (const std::uint32_t index : changed) {
                        content_bake_->node_epochs[index] = content_bake_->nodes[index]->contentEpoch();
                    }
                    refreshed_in_place = true;
                }
            }
        }

        if (!refreshed_in_place) {
            std::shared_ptr<const SceneSnapshot> previous_snapshot =
                content_bake_ != nullptr ? std::move(content_bake_->snapshot_cache) : nullptr;
            content_bake_                 = std::make_unique<SceneBake>();
            content_bake_->snapshot_cache = std::move(previous_snapshot);
            buildRows(*scene_.root_, *content_bake_, scene_.opacity_);
            content_bake_->node_epochs.resize(content_bake_->rows.size());
            for (std::size_t index = 0; index < content_bake_->rows.size(); ++index) {
                content_bake_->node_epochs[index] = content_bake_->nodes[index]->contentEpoch();
            }
        }
        bake_epoch_ = epoch;
        ++content_bake_count_;
    }
    // Byte content announces itself on the buffer, not on the graph, so the boxes that depend on it are re-read here -
    // the ONE step of a collection that still needs this thread (see refreshBoxes). A box that moved is a change the
    // EPOCH cannot see, so it moves the rows' version too: a reader that publishes these rows compares that version,
    // and would otherwise keep a stale box forever (see snapshot).
    if (refreshBoxes(*content_bake_) > 0U) {
        ++box_generation_;
    }
    // WHAT A PUBLICATION IS TOLD. The two numbers above are this scene's own (a query wants them current); the pair
    // below is the version a publication compares, and it only advances OUTSIDE an edit batch (see beginEdit): a frame
    // that lands in the middle of one publishes nothing, so the publication keeps the state the batch started from, and
    // the whole batch becomes ONE publication in the frame after the outermost endEdit.
    if (edit_depth_ == 0 && refreshValues(*content_bake_) > 0U) {
        // The values a row carries moved (see refreshValues): nothing on the graph announced it, so the version a
        // publication compares moves here, and the publication built from it is a new memo key for both readers.
        ++value_generation_;
    }
    if (edit_depth_ == 0) {
        published_epoch_  = bake_epoch_;
        published_boxes_  = box_generation_;
        published_values_ = value_generation_;
    }
    return *content_bake_;
}

std::uint64_t ScenePublisher::collectCount() const noexcept
{
    return content_collect_count_;
}

std::uint64_t ScenePublisher::reuseCount() const noexcept
{
    return content_reuse_count_;
}

std::uint64_t ScenePublisher::bakeCount() const noexcept
{
    return content_bake_count_;
}

} // namespace detail

VN_GRAPHICS_NS_END
