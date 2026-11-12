#pragma once

#include <vine/graphics/CollectView.hpp>
#include <vine/graphics/DrawListSource.hpp>
#include <vine/graphics/FrameDescription.hpp>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief The collection context one drawing call states: the camera value plus the device-pixel rectangle.
 *
 * @details
 * - The ONE place a frame's request becomes the view a collector reads, so the culling, the memo key and the
 *   view-dependent features cannot be built from two different spellings of the same four numbers.
 *
 * @param draw A description whose @ref DrawDescription::camera is engaged.
 * @return The view a DrawListSource is asked with.
 */
[[nodiscard]] CollectView collectViewOf(const DrawDescription& draw) noexcept;

/**
 * @brief Points every unbound scene drawing call of @p pass at @p source.
 *
 * @details
 * - THE ONE BIND STEP: a description states a VIEW (see RenderPass::describe) and the frame boundary says WHOSE
 *   publication answers it. Binding it here, on the side that owns the content, is what keeps a live scene out of a
 *   frame value.
 * - A draw that already names a source, a screen draw, and a draw nothing published for all stay as they are: "a
 *   list I already have" is not collected twice, and a draw nothing was published for draws nothing.
 *
 * @param pass   The pass whose scene draws are bound in place.
 * @param source The publication's source to bind, or null when nothing was published for this pass' content.
 */
void bindSceneDraws(PassDraw& pass, const vn::graphics::DrawListSource* source);

/**
 * @brief Turns the VIEWS of @p pass into command lists: the one O(drawables) step of a frame.
 *
 * @details
 * - It reads values only: each scene draw names its source (`DrawDescription::content`) and the view to collect for,
 *   and the source answers. A draw that already carries @ref commands, a screen draw, and a draw with no view are
 *   left exactly as they are - "a list I already have" is not collected twice.
 * - This is the step the threaded mode runs on the engine side: the request is O(passes), this is O(drawables), and
 *   between them there is nothing but values (see `.ai/design/graphics-threading.md`).
 *
 * @param pass The pass whose draws are expanded in place.
 */
void expandPass(PassDraw& pass);

/**
 * @brief Expands every pass of @p frame (the whole-frame form of @ref expandPass).
 *
 * @param frame The frame whose draws are expanded in place.
 */
void expandFrame(FrameDescription& frame);

} // namespace detail

VN_GRAPHICS_NS_END
