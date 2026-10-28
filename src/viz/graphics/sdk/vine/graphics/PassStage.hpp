#pragma once
#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Where a pass enters the frame's order.
 *
 * @details
 * - A frame is a list of passes the engine runs in ascending order, and a pass an EFFECT adds has to enter at the
 *   right place without knowing the numbers the passes around it happened to use: a shadow map belongs before every
 *   pass that samples it, a screen-space effect between the geometry that produced its inputs and the shading that
 *   consumes them.
 * - A stage names that place by intent — passStageOrder() turns it into the number the engine sorts by, and
 *   passes inside one stage run in the order they were planned (the engine resolves equal orders by registration
 *   order).
 * - The scale is documented rather than private so a host that registers its own pass (RenderEngine::addPass) can
 *   place it by intent too, instead of guessing where the gaps are.
 */
enum class PassStage
{
    Depth,       // Produces depth for later sampling: shadow maps, depth prepasses.
    Geometry,    // Opaque scene content (the G-buffer pass on a deferred path).
    Effect,      // Screen-space effects reading Geometry's outputs (e.g. SSAO).
    Shading,     // The lit result: the forward content pass, or the deferred lighting pass.
    Transparent, // Forward-only content composited over the shaded result.
    Present,     // Presents a baked result to the window (deferred + forward content).
    Overlay,     // HUD passes drawn over everything the frame shaded.
    Preview,     // Host / debug preview screens (PiP, attachment previews), above the HUD.
};

/**
 * @brief The order the engine sorts a pass of @p stage by.
 *
 * @details
 * - Effects state where they belong relative to the stage they serve, and this table is the one place that turns
 *   intent into the engine's sort key.
 * - The bands are spaced so a stage can gain passes later without renumbering any other; a pass inside one stage runs
 *   by registration order, so a stage needs exactly one number.
 *
 * @param stage Stage to place.
 * @return The pass order of @p stage.
 */
constexpr int passStageOrder(PassStage stage) noexcept
{
    switch (stage) {
    case PassStage::Depth: return -100;
    case PassStage::Geometry: return -40;
    case PassStage::Effect: return -20;
    case PassStage::Shading: return 0;
    case PassStage::Transparent: return 20;
    case PassStage::Present: return 40;
    case PassStage::Overlay: return 60;
    case PassStage::Preview: return 100;
    }
    return 0;
}

VN_GRAPHICS_NS_END
