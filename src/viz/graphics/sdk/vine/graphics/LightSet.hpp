#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "graphics_global.hpp"
#include "Light.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief THE LIGHTS ONE DRAW PACKET ANNOUNCES, as one immutable value: built once per change, shared by every pass
 *        of a frame.
 *
 * @details
 * - WHY A VALUE AND NOT A VECTOR PER PASS. Lights belong to the SCENE, not to a pass: a frame drawing one scene
 *   through five passes used to copy the same numbers five times per frame, and each copy was a fresh `std::vector`
 *   (`G10`: the allocation count was a function of the PASS count while the light set is a function of the scene,
 *   and the frame pipeline's own rule is zero allocations outside the frame arena). The scene builds this value once
 *   (`Scene::lightSet`), every pass holds the same shared value, and a steady frame copies nothing.
 * - IMMUTABLE AFTER CREATION (`std::shared_ptr<const LightSet>`): a frame that is already described keeps the
 *   numbers it was lit with while the content thread edits a light - the same rule `DrawPacket::light_values`
 *   and `DrawItem` follow. A new publication/description gets a new set.
 * - `LightValue` carries `identity` for the one match that needs it (`RenderTarget::shadowOf` against the announced
 *   lights); this type adds no identity of its own - it is the list, not a light.
 * - A NULL HANDLE MEANS "NOTHING ANNOUNCED" and an EMPTY SET means "there are none". They are different statements
 *   (a full-screen pass with no scene says the first; a scene with no lights says the second, which is what restores a
 *   backend's default light), which is why the field is a nullable pointer and not a vector.
 *
 * @note The packing rule is NOT here: `packLightBlock` (LightAbi.hpp) is the one function that turns the values into
 *       the shading block, so a light's term cannot mean two things.
 */
class VN_GRAPHICS_API LightSet final {
  public:
    /**
     * @brief Builds a set around @p values (the one allocation).
     *
     * @param values The lights, in announcement order (a null light already spelled as the default disabled value).
     * @return The immutable set; never null.
     */
    [[nodiscard]] static std::shared_ptr<const LightSet> create(std::vector<LightValue> values);

    LightSet(const LightSet&)            = delete;
    LightSet& operator=(const LightSet&) = delete;

  public:
    /** @brief Gets the lights, in announcement order. */
    [[nodiscard]] std::span<const LightValue> values() const noexcept;

    /** @brief Gets how many lights the set holds. */
    [[nodiscard]] std::size_t size() const noexcept;

    /** @brief Answers whether the set holds no light at all (an announced "there are none"). */
    [[nodiscard]] bool empty() const noexcept;

  private:
    explicit LightSet(std::vector<LightValue> values);

  private:
    std::vector<LightValue> values_;
};

/// @brief The shared handle a frame value carries (see `DrawPacket::light_values`).
using LightSetPtr = std::shared_ptr<const LightSet>;

VN_GRAPHICS_NS_END
