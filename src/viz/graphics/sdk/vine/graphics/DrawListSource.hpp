#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include "CollectView.hpp"
#include "RenderCommand.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief The ONE question a frame asks about a drawable set: "what would this view draw, from you".
 *
 * @details
 * - WHY AN INTERFACE. A frame's shape is a set of VIEWS (see DrawDescription), and the commands behind a view come
 *   from something that owns the content. Two such owners exist and must answer identically: a `Scene` (the authored
 *   graph, which answers on the content thread) and a `SnapshotSource` (an immutable publication, which answers
 *   anywhere). Naming the question here is what lets the frame machinery ask it without knowing which side answers.
 * - IT IS A VIEW QUERY, NOT A SCENE QUERY. The answer is a pure function of the source's state and the view: one
 *   implementation may memoise it per epoch and per view (Scene does), another may hold no graph at all (a
 *   publication selects from its own rows). Neither is part of this contract.
 * - THE ANSWER IS SHARED AND IMMUTABLE. Every pass of a frame that draws the same view hands the same list to the
 *   backend, and a command is never rewritten per pass (a pass' overrides are resolved by the backend, see
 *   `PassDraw`). The returned value is what keeps that list alive for the frame.
 *
 * @warning
 * - Called from the thread that owns the frame's collection. A `Scene` answers from the graph it owns (so it answers
 *   on the content thread); a `SnapshotSource` answers from an immutable value (so it answers on the engine thread).
 *   Do not call one from the other's thread.
 */
class VN_GRAPHICS_API DrawListSource {
  public:
    DrawListSource()          = default;
    virtual ~DrawListSource();

    DrawListSource(const DrawListSource&)            = delete;
    DrawListSource& operator=(const DrawListSource&) = delete;

    /**
     * @brief Collects what @p view draws from this source, restricted to @p layer_mask.
     *
     * @param view       The view to collect for (projection / view / eye / up / device-pixel viewport).
     * @param layer_mask Layers this picture draws (see Node::setLayers): part of what makes two views differ.
     * @return The commands, in draw order; empty when nothing is drawn. Shared because every pass of a frame that
     *         draws this view names the same list.
     */
    [[nodiscard]] virtual std::shared_ptr<const std::vector<RenderCommand>> collectRenderCommands(const CollectView& view,
                                                                                                 std::uint32_t       layer_mask) const = 0;
};

VN_GRAPHICS_NS_END
