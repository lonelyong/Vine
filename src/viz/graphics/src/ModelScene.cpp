#include <vine/graphics/ModelScene.hpp>

#include <algorithm>
#include <cmath>
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

/// The perceived roughness a Blinn-Phong exponent corresponds to (graphics-material.md §3.6): the standard
/// Blinn-Phong -> GGX mapping, stated once here because this file is the ONLY place a Phong source becomes an
/// engine material.
///
/// - `n = 0` (a highlight with no falloff) is fully rough, and a mirrored surface (an exponent in the thousands)
///   approaches 0; the engine adds no clamp of its own, because a shader clamps what it draws.
/// - The M1 shading bridge inverts this exact formula, so an imported asset keeps the highlight width the source
///   gave it while the real BRDF is still to come.
[[nodiscard]] float roughnessFromShininess(float shininess) noexcept
{
    const float exponent = std::max(shininess, 0.0F);
    return std::clamp(std::sqrt(2.0F / (exponent + 2.0F)), 0.0F, 1.0F);
}

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
struct Conversion {
    const vn::geometry::Model&                                     model;
    ModelScene&                                                    scene;
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
        on_path[index]                          = true;

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
                leaf->setMaterial(sharedMaterial(*entry.material));
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
    case vn::geometry::MaterialType::Color:
    {
        // A colour-only source has no highlight of its own: the colour IS the material, and fully rough means the
        // engine adds no gloss nobody authored (graphics-material.md §3.6).
        const Colorf base = static_cast<const vn::geometry::ColorMaterial&>(material).color();
        converted->setBaseColor(base);
        converted->setMetallic(0.0F);
        converted->setRoughness(1.0F);
        break;
    }
    case vn::geometry::MaterialType::Phong:
    {
        // THE FIXED TABLE (graphics-material.md §3.6) - the one place Phong exists at all:
        // - the diffuse reflectance is the base colour;
        // - the exponent becomes the roughness through the standard mapping, so the highlight keeps its width;
        // - the specular colour becomes metallic. Metallic-roughness has no separate reflectance knob, and a
        //   strong or tinted specular is exactly what "metal" means in it: the strongest channel is the
        //   reflection's peak and the alpha scales it. A dielectric's Ks (0.04-0.08) therefore lands near 0, a
        //   mirror's 1 lands on 1.
        const auto&  phong    = static_cast<const vn::geometry::PhongMaterial&>(material);
        const Colorf specular = phong.specular();
        const float  peak     = std::max({ specular.r, specular.g, specular.b });
        converted->setBaseColor(phong.diffuse());
        converted->setRoughness(roughnessFromShininess(phong.shininess()));
        converted->setMetallic(std::clamp(peak * std::clamp(specular.a, 0.0F, 1.0F), 0.0F, 1.0F));
        break;
    }
    case vn::geometry::MaterialType::Pbr:
    {
        // Field for field, with no approximation: the source already speaks the engine's vocabulary. Opacity stays
        // the LEAF's (a shared material's alpha would make every drawable that uses it translucent), which the
        // caller applies.
        const auto&  pbr      = static_cast<const vn::geometry::PbrMaterial&>(material);
        const Colorf emissive = pbr.emissive();
        converted->setBaseColor(pbr.baseColor());
        converted->setMetallic(std::clamp(pbr.metallic(), 0.0F, 1.0F));
        converted->setRoughness(std::clamp(pbr.roughness(), 0.0F, 1.0F));
        converted->setEmissive(Colorf(emissive.r, emissive.g, emissive.b, 1.0F));
        break;
    }
    default: break;
    }

    return converted;
}

VN_GRAPHICS_NS_END
