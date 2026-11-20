#pragma once
#include "graphics_global.hpp"

#include <cstdint>

#include <vine/Colorf.hpp>

#include "Geometry.hpp"

VN_GRAPHICS_NS_BEGIN

/// @brief The +X arrow of a coordinate-frame marker (see AxesStyle::axis_mask).
inline constexpr std::uint32_t kAxisX = 1u << 0;
/// @brief The +Y arrow of a coordinate-frame marker.
inline constexpr std::uint32_t kAxisY = 1u << 1;
/// @brief The +Z arrow of a coordinate-frame marker.
inline constexpr std::uint32_t kAxisZ = 1u << 2;
/// @brief Every arrow of a coordinate-frame marker.
inline constexpr std::uint32_t kAllAxes = kAxisX | kAxisY | kAxisZ;

/// @brief A shaft's radius as a fraction of the same arrow's tip radius (`AxesStyle::shaft_radius` vs `tip_radius`).
inline constexpr double kAxesShaftToTipRatio = 0.4;

/**
 * @brief How a coordinate-frame marker looks: one arrow per axis, each a cylinder shaft closed by a cone tip.
 *
 * @details
 * - The shape is the one VTK's axes actor describes (total length, shaft radius, tip radius, tip length, resolution),
 *   and the convention is the one a robot visualiser uses for a joint frame: three arrows in arrow-colour order
 *   X = red, Y = green, Z = blue, of one length, drawn as ONE geometry.
 * - The radii and the tip length are FRACTIONS OF `length`, so one style describes a marker at any scale: a 128-pixel
 *   HUD gizmo and a 0.1 m joint frame are the same values in a different unit.
 * - `length` is the distance from the origin to an arrow's tip. A non-positive length builds nothing to draw, as does
 *   an empty `axis_mask`.
 * - Every other field is CLAMPED by `makeAxesMarker()` into a range a surface can be built from (a marker is
 *   decoration: an unusable value is corrected, never reported as an error).
 *
 * @note
 * - The colours are per-vertex, not per-draw material: the three arrows are one primitive and one draw (see
 *   makeAxesMarker), and the engine's shading multiplies a draw's colour by the vertex colour.
 * - `axis_mask` is a BUILD parameter: hiding an axis is a different marker, not a runtime switch. A caller that needs
 *   both states keeps two geometries (they are cheap - see the prototype sharing in makeAxesMarker).
 */
struct VN_GRAPHICS_API AxesStyle {
    /// Distance from the origin to a tip, in world units.
    double length{ 1.0 };
    /// Shaft radius, as a fraction of `length` (the default is `kAxesShaftToTipRatio` of `tip_radius`).
    double shaft_radius{ 0.03 };
    /// Tip (cone base) radius, as a fraction of `length`.
    double tip_radius{ 0.075 };
    /// Tip (cone) length, as a fraction of `length`; at least one tenth of the arrow is shaft.
    double tip_length{ 0.25 };
    /// Azimuth intervals of both the shaft and the tip; clamped to at least 3.
    int segments{ 16 };
    /// Which arrows to build: `kAxisX`, `kAxisY`, `kAxisZ`, or any combination of them.
    std::uint32_t axis_mask{ kAllAxes };
    /// Colour of the +X arrow (linear RGBA).
    Colorf x_color{ 0.90f, 0.20f, 0.20f, 1.0f };
    /// Colour of the +Y arrow (linear RGBA).
    Colorf y_color{ 0.20f, 0.80f, 0.20f, 1.0f };
    /// Colour of the +Z arrow (linear RGBA).
    Colorf z_color{ 0.20f, 0.40f, 0.95f, 1.0f };

    /**
     * @brief Compares every field exactly.
     *
     * @details
     * - Exact, not approximate: a style is a caller's description, and the streams behind a marker are shared on
     *   this comparison. Two styles that differ in the last bit are two markers, which costs one more prototype and
     *   is never wrong.
     *
     * @param left  Style to compare.
     * @param right Style to compare with.
     * @return true when every field is equal.
     */
    [[nodiscard]] friend bool operator==(const AxesStyle& left, const AxesStyle& right) noexcept = default;
};

/**
 * @brief Builds the geometry of one coordinate-frame marker: the arrows @p style names, as ONE draw.
 *
 * @details
 * - A MARKER is decoration, not content: it says where a frame is, it is not part of what the frame models. That is
 *   why this returns a plain geometry the caller parents wherever it wants, and why the arrows are one draw rather
 *   than three.
 * - ONE GEOMETRY, ONE PRIMITIVE, ONE UPLOAD. The arrows share one position/normal/colour stream and are told apart by
 *   their vertex colour, so a marker costs one draw however many axes it shows. A caller that needs an axis hidden
 *   builds another marker (see AxesStyle::axis_mask).
 * - THE TRIANGLES ARE GEOM'S. Each arrow is `vn::geom::spatial::tessellateSolid()`'s cylinder and cone, rotated from
 *   +Z onto its axis. The geom module owns what a shaft and a tip are; this function owns only where they go.
 * - THE STREAMS ARE SHARED PER STYLE. Every call returns a NEW geometry NODE (a node has one parent, so two
 *   placements cannot be one node - see Group::addChild) that READS the streams of a prototype built once for that
 *   style, so hundreds of markers cost one set of vertex buffers and one device upload. A style's prototype is
 *   immutable and held for the process; the amount of styles kept is bounded, and a style past the bound is built
 *   without being kept (the caller's geometry still shares nothing but its own streams).
 * - The material is the marker's too: a white diffuse one, so the vertex colours are what reach the frame - a caller
 *   that wants the marker shaded like its scene sets its own material on the returned geometry.
 * - NOTHING HERE READS A CLOCK OR A CAMERA: the transform of a marker is its node's, and the scale is the style's.
 *
 * @warning
 * - The streams are shared, the DRAW is not: each returned geometry owns its primitive list, so an edit to one
 *   marker's primitive (its material, its program, its index span) never reaches another. Editing its CHANNELS goes
 *   through `setPositions` and friends, which attach a new buffer to THAT marker only.
 *
 * @param style How the marker looks (see AxesStyle).
 * @return A geometry holding the style's arrows; an empty geometry (no channels, no draw) when the style names no
 *         axis or has a non-positive length.
 */
[[nodiscard]] VN_GRAPHICS_API GeometryPtr makeAxesMarker(const AxesStyle& style = {});

VN_GRAPHICS_NS_END
