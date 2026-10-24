#pragma once
#include "graphics_global.hpp"

#include <cstddef>

#include <vine/intrusive_ptr.hpp>

#include "Material.hpp"
#include "Node.hpp"

/// The geometry module's data model, borrowed here so this header stays a graphics header (see sceneFromModel).
namespace vn::geometry
{
class Model;
class Material;
} // namespace vn::geometry

VN_GRAPHICS_NS_BEGIN

/**
 * @brief What sceneFromModel() built: the node tree, and how much of the model it could turn into draws.
 *
 * @details
 * - Every count is a fact about the CONVERSION, not about the model: an entry with no mesh, or one the graphics layer
 *   cannot turn into vertex data, is skipped and counted instead of aborting the walk, so one odd part of an
 *   imported file cannot cost the rest of it.
 */
struct VN_GRAPHICS_API ModelScene
{
    /// The model's root as a scene node, or null when the model has no nodes (nothing to draw).
    NodePtr root;
    /// How many `Geometry` leaves the tree holds (one per (node, mesh entry) pair).
    std::size_t geometries{0};
    /// How many mesh entries were skipped because they do not describe vertex data the engine can draw.
    std::size_t skipped_meshes{0};
    /// How many node references were skipped because they were out of range or closed a cycle.
    std::size_t skipped_nodes{0};
};

/**
 * @brief Turns a loaded model into scene nodes: the last hop between an asset and a picture.
 *
 * @details
 * - THE SHAPE OF THE RESULT is the model's: a node with a non-identity `transform` becomes a `MatrixTransform`,
 *   one without a `Group` (both of which are also what carries the node name), each mesh entry of a node becomes
 *   one `Geometry` under it, and children are attached in the order the model lists them.
 * - VERTEX DATA IS SHARED, NODES ARE NOT: every `Geometry` is a separate leaf (the scene graph is a strict tree,
 *   see Group::addChild), but the buffers come from `geometryFromShape`, so a mesh used by two nodes is uploaded
 *   once. A model that reuses one mesh in ten places therefore costs ten leaves and one mesh's worth of memory.
 * - MATERIALS ARE SHARED BY SOURCE IDENTITY: one `geometry::Material` referenced by several entries becomes ONE
 *   `graphics::Material` (the shading values are the material's, and a shared material is what lets the backend
 *   batch the draws that use it).
 * - NAMES TRAVEL: the node name, the mesh name and the material name are copied, which is what makes a converted
 *   tree readable in Scene::findNode() and in diagnostics.
 * - A MALFORMED MODEL IS COUNTED, NOT REPAIRED: an out-of-range index, an entry with no mesh and a reference that
 *   closes a loop are skipped and counted (ModelScene::skipped_nodes / skipped_meshes) - the walk never recurses
 *   forever and the rest of the tree still converts. A node referenced TWICE is not malformed: it becomes two
 *   containers (the scene graph is a tree), and their leaves still share the mesh's buffers.
 *
 * @note
 * - This is the counterpart of `geometryFromShape` at the MODEL level: it lives in graphics (not in meshio) so a
 *   consumer that already holds a model does not have to link the loader - the same reason the model itself lives
 *   in geometry rather than in meshio.
 * - What it does NOT do: it builds no `Scene` (the caller owns the root and decides which scene it belongs to),
 *   it applies no global scale (the loader's option already folded that into the root transform), and it creates
 *   no light.
 *
 * @param model Model to convert; an empty model yields a null root.
 * @return The built tree and the conversion's counts.
 */
VN_GRAPHICS_API ModelScene sceneFromModel(const vn::geometry::Model& model);

/**
 * @brief Converts one geometry material into the material a draw uses.
 *
 * @details
 * - PBR translates field for field (base colour, metallic, roughness, emissive): the engine's model IS the source's
 *   model, so nothing is approximated.
 * - Color becomes the base colour with `metallic = 0` and `roughness = 1` (a flat colour authors no gloss).
 * - Phong is the ONE conversion: the diffuse reflectance becomes the base colour, the exponent becomes the roughness
 *   through `sqrt(2 / (n + 2))` (the standard Blinn-Phong -> GGX mapping, so the highlight keeps its width), and the
 *   specular colour becomes metallic - its strongest channel scaled by its intensity. Metallic-roughness has no
 *   separate reflectance knob, so a strong or tinted specular is what "metal" means there; the trade-off is stated in
 *   graphics-material.md §3.6.
 * - `PbrMaterial::opacity` is NOT part of the material - the engine's transparency is the node's opacity - so
 *   `sceneFromModel` puts it on the leaf instead (see Node::setOpacity).
 * - The result is a NEW object every call; a caller converting a whole model does not call this per entry (that
 *   would give one material object per draw instead of one per source material) - `sceneFromModel` caches by the
 *   source object's identity.
 *
 * @param material Material to convert.
 * @return The graphics material for that source material.
 */
VN_GRAPHICS_API intrusive_ptr<Material> materialFrom(const vn::geometry::Material& material);

VN_GRAPHICS_NS_END
