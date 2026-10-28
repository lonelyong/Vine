#pragma once
#include <cstddef>

#include <vine/vsg/internal/ContentFacts.hpp>
#include <vine/vsg/vsg_global.hpp>

VN_VSG_NS_BEGIN

/**
 * @brief What is wrong with ONE material texture slot, asked of the map the slot holds.
 *
 * @details
 * - The slot's contract (graphics-material.md 2.9 / C2) is about the MAP, not the shader: base colour and
 *   emissive are sampled as COLOUR (their bytes are sRGB-encoded and the hardware decodes them), the other
 *   three are DATA (linear bytes read as numbers), and only the base colour may be a cube map - the other slots
 *   parameterise a surface with a UV pair, which a direction is not.
 * - A violation is a REPORT, not a refusal: the slot still draws, with the map that is there, because dropping
 *   a drawable over a colour space would be worse than a gamma-shifted picture. The one case the content layer
 *   cannot bind at all is the KIND (a cube view where the text declares `sampler2D` is an invalid descriptor),
 *   and that one falls back to the declared kind's white and is counted - see ContentSets.
 *
 * @note
 * - Judged from the FACTS the content layer already has (`MaterialFacts::slots`), so the same check serves the
 *   record-time report and a test with no device.
 * - A format it cannot judge (`Unknown`, a float format in a colour slot, a depth format) is not a violation:
 *   HDR colour is legitimately linear, and guessing would make the report noise.
 */
enum class SlotViolation
{
    None,                    // The slot is absent, or its map fits it.
    CubeInDataSlot,          // A cube map where the slot takes a 2-D data map (C2/N6).
    LinearFormatInColorSlot, // A byte map claiming to be linear in a slot sampled as colour.
    SrgbFormatInDataSlot,    // An sRGB map in a slot read as data (roughness would be gamma-decoded).
};

/**
 * @brief Checks one slot of @p material against its contract.
 *
 * @param material The material's facts (its slot maps and their kinds/formats).
 * @param slot     Slot index, in `vn::graphics::TextureSlot` order.
 * @return What is wrong with that slot, or SlotViolation::None (including "the slot has no map").
 */
[[nodiscard]] SlotViolation materialSlotViolation(const MaterialFacts& material, std::size_t slot) noexcept;

/**
 * @brief Gets the slot's name from the ABI's vocabulary (the name its binding is resolved by).
 *
 * @param slot Slot index, in `vn::graphics::TextureSlot` order.
 * @return The name (`vn_baseColorMap`, ...), or `nullptr` when the index is not a slot.
 */
[[nodiscard]] const char* materialSlotName(std::size_t slot) noexcept;

/**
 * @brief Gets the sentence that names @p violation, for the diagnostic a host reads.
 *
 * @param violation The violation to describe.
 * @return A one-clause description, or an empty string for SlotViolation::None.
 */
[[nodiscard]] const char* slotViolationText(SlotViolation violation) noexcept;

VN_VSG_NS_END
