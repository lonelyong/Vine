#pragma once
#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief What a chain's picture MEANS: the viewing mode of a viewer (see PassChain::setShadingMode).
 *
 * @details
 * - Every mode is the SAME built-in forward program text with one `#define` (see BuiltinShaders),
 *   applied as the PICTURE pass' program override - the pass that turns geometry into the picture - and
 *   Wireframe adds that pass' rasterisation mode on top. A mode is therefore a PASS PROPERTY: one the
 *   current chain can express costs an override and never a rebuild, and that is what makes switching
 *   modes interactive.
 * - IT IS DECLARED AT BUILD TIME AND SWITCHED AT RUNTIME. What a chain can express depends on the
 *   chain: a Deferred picture comes out of a fullscreen lighting pass that has no geometry left to
 *   shade, so a non-Lit mode has to be ASSEMBLED for it (ChainDesc::mode; the builder then builds
 *   the chain that can express it - see PassChainBuilder::effectivePath), while switching among
 *   the modes that chain CAN express is one override on a live chain.
 * - PassChain::setShadingMode() answers which of the two a request needs.
 * - It lives here (rather than on a view) because a mode is a property of the chain that draws the
 *   picture: the chain owns the picture pass the override lands on, and two views drawn by two
 *   chains can be showing two different modes at once.
 */
enum class ShadingMode
{
    Lit,       ///< The full shading: ambient + directional lights + specular.
    Flat,      ///< The same lighting with the per-FACE normal (faceted surfaces, no vertex normals read).
    Unlit,     ///< The surface colour alone (material / map / vertex colour), no light term.
    Normals,   ///< View-space normals as colour: which way each surface faces.
    Wireframe, ///< Edges only (RenderPass::setPolygonModeOverride), in the surface's own colour.
};

VN_GRAPHICS_NS_END
