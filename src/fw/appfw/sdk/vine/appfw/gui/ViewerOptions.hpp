#pragma once

#include <cstdint>

#include <vine/graphics/Node.hpp>
#include <vine/graphics/ShadingMode.hpp>
#include <vine/graphics/ShadingPath.hpp>

#include <vine/appfw/appfw_global.hpp>

VN_APPFWGUI_NS_BEGIN

/**
 * @brief How the area draws: the recipe the viewer builds its chain from.
 *
 * WHAT THIS IS. A render control gives a host a 3D area it does not have to wire itself: the viewer (Viewer)
 * owns one request, builds the area's pipeline from it (RenderPipelineBuilder with the options derived
 * from here) and adopts it on the area's view. This is the host-facing half of the recipe - the choices the area
 * has - while the CONTEXTUAL fields of PipelineOptions (the gizmo's source camera = the area's camera, the pixel
 * ratio both overlays want = the area's ratio) are filled in where the chain is built. A render control's
 * viewerOptions / setViewerOptions / setShadingMode are shortcuts onto the area's viewer.
 *
 * WHAT IT IS NOT. It is not the pipeline contract: a host that wants more (its own G-buffer or lighting
 * programs, a chain of its own) builds a Pipeline with RenderPipelineBuilder and adopts it (SceneView::setPipeline)
 * - that view's drawing is then the host's, and this request no longer applies to it.
 *
 * CHANGING IT REBUILDS. A chain, an overlay's existence and the shading mode the chain must be able to express
 * are all build decisions (see PipelineOptions), so a change here is one pipeline build - not a
 * per-frame cost, but not free either. What costs nothing is a mode the built chain can express and the
 * visibility of an overlay it has: those are Pipeline::setShadingMode / setGizmoVisible / setStatsVisible, calls
 * on the live pipeline.
 */
struct VN_APPFW_API ViewerOptions
{
    /// How the opaque content is shaded (the structural choice; see ShadingPath).
    vn::graphics::ShadingPath path{ vn::graphics::ShadingPath::Forward };

    /// What the picture MEANS (see ShadingMode). A non-Lit mode is a geometry-stage override, so a Deferred
    /// request carrying one is built FORWARD for it (RenderPipelineBuilder::effectivePath) - the requested path
    /// is kept, so switching back to Lit builds deferred again.
    vn::graphics::ShadingMode mode{ vn::graphics::ShadingMode::Lit };

    /// Whether the area HAS an axis-gizmo overlay (its visibility is a switch on the built chain).
    bool gizmo{false};

    /// Whether the area HAS a frame-rate readout (its visibility is a switch on the built chain).
    bool stats{false};

    /// Deferred G-buffer size in pixels; 0 (the default) is the current surface size.
    int offscreen_width{0};
    int offscreen_height{0};

    /// Which layers of the content the area draws (see Node::setLayers / PipelineOptions::layer_mask).
    ///
    /// The area's own content selection, stated on every pass of the chain that walks the content: an area
    /// showing one part of a scene while another area (or another pass) shows the rest. kAllLayers (the
    /// default) draws every layer, so a host that never heard of layers is unaffected.
    std::uint32_t layer_mask{ vn::graphics::kAllLayers };
};

VN_APPFWGUI_NS_END
