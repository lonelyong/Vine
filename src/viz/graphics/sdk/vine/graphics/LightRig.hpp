#pragma once
#include "graphics_global.hpp"

#include <vector>

#include <vine/Colorf.hpp>
#include <vine/math/Vector3.hpp>

VN_GRAPHICS_NS_BEGIN

class Scene;

/**
 * @brief A named set of lights, added to a scene in one call.
 *
 * A scene with no light is not dark: the backends substitute a low ambient term so geometry stays visible
 * (measured: albedo * 0.15, i.e. flat colour). That fallback is why "why is my model flat?" is an easy question to
 * end up asking, and the answer is always the same - nobody added a light. This is the one-liner that adds a
 * whole rig, so the answer stops being a paragraph in a document.
 *
 * A rig holds DESCRIPTIONS, not lights: addTo() creates fresh Light objects every time, so one `studio()` can
 * light any number of scenes and no Light object is shared behind the caller's back.
 *
 * The presets are the two conventions every viewer starts with:
 *
 *  - studio(): the classic three-point set (one ambient fill + a key and a fill from opposite sides), which
 *    gives shape to an arbitrary model without the host knowing where its camera is. This is the safe default.
 *  - headlight(direction): one directional light travelling along the camera's view direction, the "I can always
 *    see what I am looking at" rig (VTK's automatic light is this one, created implicitly; here it is explicit).
 *
 * Where to put them is the scene's business: `scene.addLighting(LightRig::studio())` appends, and
 * `scene.setLighting(...)` replaces whatever the scene had.
 */
class VN_GRAPHICS_API LightRig {
  public:
    /**
     * @brief One light of a rig: what addTo() creates.
     */
    struct Entry {
        Colorf          color{};                ///< RGB colour in [0,1] (alpha ignored).
        float           intensity{ 1.0f };       ///< Intensity multiplier (1 is neutral).
        bool            directional{ false };   ///< true: directional along @ref direction; false: ambient.
        vn::math::Vec3d direction{};            ///< Travel direction in world space (directional entries only).

        /// @brief Returns whether this entry's direction is usable (a directional light pointing nowhere lights
        /// nothing).
        bool hasDirection() const noexcept
        {
            return direction.length() > 0.0;
        }
    };

  public:
    /**
     * @brief Builds the three-point studio rig: one ambient fill plus a key and a fill light from opposite sides.
     *
     * @return The rig (1 ambient + 2 directional entries, all directions non-zero and normalized).
     */
    static LightRig studio();

    /**
     * @brief Builds a single headlight travelling along @p direction.
     *
     * @param direction View direction of the camera being lit (world space); defaults to looking down -Z, which
     *                  is where a camera starts.
     * @return The rig (1 directional entry).
     */
    static LightRig headlight(const vn::math::Vec3d& direction = vn::math::Vec3d(0.0, 0.0, -1.0));

  public:
    /**
     * @brief Adds this rig's lights to @p scene, leaving the lights already there alone.
     *
     * Creates one Light per entry (a directional one carries the entry's direction, colour and intensity; an
     * ambient one carries the entry's colour and intensity) and hands each to Scene::addLight().
     *
     * @param scene Scene to light; it keeps its own reference to every light created.
     */
    void addTo(Scene& scene) const;

    /**
     * @brief Gets this rig's entries, in the order addTo() creates them.
     *
     * @return The entries.
     */
    const std::vector<Entry>& lights() const noexcept;

  private:
    std::vector<Entry> lights_;
};

VN_GRAPHICS_NS_END
