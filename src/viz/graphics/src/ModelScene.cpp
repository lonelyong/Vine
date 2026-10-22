#include <vine/graphics/ModelScene.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <vine/geometry/ColorMaterial.hpp>
#include <vine/geometry/Material.hpp>
#include <vine/geometry/Mesh.hpp>
#include <vine/geometry/Model.hpp>
#include <vine/geometry/PbrMaterial.hpp>
#include <vine/geometry/PhongMaterial.hpp>
#include <vine/math/Matrix4x4.hpp>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/MatrixTransform.hpp>

VN_GRAPHICS_NS_BEGIN

namespace
{
/**
 * @brief Puts @p material on every primitive of @p geometry (adding one when it has none).
 *
 * The material is the DRAW's (see Primitive.hpp): a leaf built from one shape has one primitive, and a
 * caller that only holds the geometry assigns through it instead of keeping a second spelling on the leaf.
 *
 * @param geometry Geometry to shade.
 * @param material Material to assign.
 */
void assignMaterial(Geometry& geometry, intrusive_ptr<Material> material)
{
    if (geometry.primitiveCount() == 0) {
        Primitive draw;
        draw.material = std::move(material);
        geometry.addPrimitive(std::move(draw));
        return;
    }
    for (std::size_t index = 0; index < geometry.primitiveCount(); ++index) {
        Primitive draw = *geometry.primitive(index);
        draw.material  = material;
        geometry.setPrimitive(index, std::move(draw));
    }
}
}  // namespace


namespace
{

/// The share of a flat colour a ColorMaterial's ambient term gets: the engine's own default ambient is 0.2 grey,
/// so a colour-only source keeps the same "unlit parts are dark, not black" look its Phong sibling would have.
constexpr float kColorAmbientShare = 0.2F;
/// Phong exponents a PBR roughness maps onto: fully rough stays nearly matte, fully smooth is a tight highlight.
constexpr float kPbrShininessFloor = 1.0F;
constexpr float kPbrShininessRange = 127.0F;

/**
 * @brief One conversion's state: the model, the result being filled and the caches that make it a tree.
 *
 * @details
 * - The material cache is keyed by the SOURCE object: a material referenced by two entries must reach the backend
 *   as one object, or every drawable gets its own slot and the batches that share a material stop being batches.
 * - `on_path` is the cycle guard: a model whose node references form a loop would make this walk recurse forever,
 *   and the scene graph it is building forbids cycles anyway (see Group::addChild) - so the loop is cut at the
 *   second visit and counted.
 */
struct Conversion
{
    const vn::geometry::Model& model;
    ModelScene&                scene;
    std::unordered_map<const vn::geometry::Material*, MaterialPtr> materials;
    std::vector<bool>                                              on_path;

    /// @brief Builds one node and its subtree, or returns null for a reference that cannot be built.
    NodePtr node(std::uint32_t index)
    {
        if (index >= model.nodes().size() || on_path[index]) {
            ++scene.skipped_nodes;
            return NodePtr();
        }
        const vn::geometry::Model::Node& source = model.nodes()[index];
        on_path[index] = true;

        intrusive_ptr<Group> group;
        if (source.transform.isIdentity()) {
            group = make_intrusive<Group>();
        }
        else {
            auto transform = make_intrusive<MatrixTransform>();
            transform->setMatrix(source.transform);
            group = transform;
        }
        group->setName(source.name);

        // The meshes of THIS node first, then its children: a node's own parts are drawn before the subtrees it
        // carries, which is the order a reader of the model expects (the source lists both independently).
        for (const std::uint32_t entry_index : source.meshes) {
            if (entry_index >= model.meshes().size()) {
                ++scene.skipped_meshes;
                continue;
            }
            const vn::geometry::Model::Entry& entry = model.meshes()[entry_index];
            if (entry.mesh == nullptr) {
                ++scene.skipped_meshes;
                continue;
            }
            // A shape that is not a vertex mesh (a Brep, a primitive) is not drawable here; one odd part must not
            // cost the rest of the model.
            intrusive_ptr<Geometry> leaf = geometryFromShape(*entry.mesh);
            if (leaf == nullptr) {
                ++scene.skipped_meshes;
                continue;
            }
            leaf->setName(entry.mesh->name());
            if (entry.material != nullptr) {
                assignMaterial(*leaf, sharedMaterial(*entry.material));
                // PBR carries a per-material opacity; the engine's transparency is the NODE's (a shared material's
                // alpha would make every drawable translucent), so it is put on the leaf here.
                if (const auto* pbr = dynamic_cast<const vn::geometry::PbrMaterial*>(entry.material.get())) {
                    const float opacity = std::clamp(pbr->opacity(), 0.0F, 1.0F);
                    if (opacity < 1.0F) {
                        leaf->setOpacity(opacity);
                    }
                }
            }
            group->addChild(leaf);
            ++scene.geometries;
        }

        for (const std::uint32_t child_index : source.children) {
            if (NodePtr child = node(child_index)) {
                group->addChild(std::move(child));
            }
        }

        on_path[index] = false;
        return group;
    }

    /// @brief Answers the graphics material for one source material, converting it once.
    MaterialPtr sharedMaterial(const vn::geometry::Material& source)
    {
        const auto found = materials.find(&source);
        if (found != materials.end()) {
            return found->second;
        }
        MaterialPtr converted = materialFrom(source);
        materials.emplace(&source, converted);
        return converted;
    }
};

} // namespace

ModelScene sceneFromModel(const vn::geometry::Model& model)
{
    ModelScene scene;
    if (model.nodes().empty()) {
        return scene;
    }
    if (model.root() >= model.nodes().size()) {
        ++scene.skipped_nodes;
        return scene;
    }

    Conversion conversion{ model, scene, {}, std::vector<bool>(model.nodes().size(), false) };
    scene.root = conversion.node(model.root());
    return scene;
}

intrusive_ptr<Material> materialFrom(const vn::geometry::Material& material)
{
    auto converted = make_intrusive<Material>();
    converted->setName(material.name());

    switch (material.materialType()) {
    case vn::geometry::MaterialType::Color: {
        // A colour-only source has no highlight of its own: the ambient share keeps its unlit parts readable, and
        // the zero-strength specular keeps the engine from inventing one.
        const Colorf base = static_cast<const vn::geometry::ColorMaterial&>(material).color();
        converted->setDiffuse(base);
        converted->setAmbient(Colorf(base.r * kColorAmbientShare, base.g * kColorAmbientShare,
                                     base.b * kColorAmbientShare, 1.0F));
        converted->setSpecular(Colorf(0.0F, 0.0F, 0.0F, 0.0F));
        break;
    }
    case vn::geometry::MaterialType::Phong: {
        const auto& phong = static_cast<const vn::geometry::PhongMaterial&>(material);
        converted->setAmbient(phong.ambient());
        converted->setDiffuse(phong.diffuse());
        converted->setSpecular(phong.specular());
        converted->setShininess(phong.shininess());
        break;
    }
    case vn::geometry::MaterialType::Pbr: {
        // THE APPROXIMATION, spelled out where a reader looks for it (see materialFrom's note): a metal has no
        // diffuse lobe, its highlight takes the base colour, and smoothness becomes both the highlight's strength
        // and the Phong exponent. It is a faithful ENOUGH picture for an imported asset, not a PBR shading model.
        const auto&   pbr       = static_cast<const vn::geometry::PbrMaterial&>(material);
        const float   metallic  = std::clamp(pbr.metallic(), 0.0F, 1.0F);
        const float   roughness = std::clamp(pbr.roughness(), 0.0F, 1.0F);
        const Colorf  base      = pbr.baseColor();
        const Colorf  highlight = Colorf::lerp(Colorf(1.0F, 1.0F, 1.0F, 1.0F), base, metallic);
        const float   smoothness = 1.0F - roughness;
        converted->setDiffuse(Colorf(base.r * (1.0F - metallic), base.g * (1.0F - metallic),
                                     base.b * (1.0F - metallic), base.a));
        converted->setSpecular(Colorf(highlight.r, highlight.g, highlight.b, smoothness));
        converted->setShininess(kPbrShininessFloor + smoothness * kPbrShininessRange);
        break;
    }
    default: break;
    }

    return converted;
}

VN_GRAPHICS_NS_END
