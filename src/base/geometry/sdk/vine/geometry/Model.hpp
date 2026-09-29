#pragma once

#include "geometry_global.hpp"

#include <cstdint>
#include <vector>

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>

#include "Material.hpp"
#include "Mesh.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A model as its source described it: a node tree, meshes and materials.
 *
 * A model keeps the structure a flat mesh loses. The nodes form a tree (index
 * 0 is the root by convention, see root()); each node carries a name and a
 * transform that places its children, and each mesh entry pairs a mesh with the
 * material it uses. A node's meshes may be shared with another node - the same
 * mesh drawn in two places is still one mesh.
 *
 * The meshes are in their own local space: a mesh holds the vertices the source
 * gave and a node's transform places them, so nothing is baked twice. Reading a
 * model as one flat mesh with the transforms applied is what
 * MeshLoader::loadMergedMesh() does.
 *
 * Names live where they belong: a mesh is named through Shape::name() and a
 * material through Material::name(), so the model itself stores no strings
 * beyond the node names.
 *
 * The setters exist for the reader that fills a model in; a holder only reads
 * it (the loader builds a model once and hands it out through an intrusive_ptr,
 * so one model is shared rather than copied).
 */
class VN_GEOMETRY_API Model : public vn::Object, public vn::RefCounted<Model> {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief One node of the tree: a name, a local transform and what it refers to.
     *
     * Every reference is an index into the arrays of the model that holds the
     * node, which is why a node on its own means nothing.
     */
    struct Node {
        String                     name;      ///< Node name; empty when the source named none.
        math::Mat4d                transform; ///< Transform of the node relative to its parent.
        std::vector<std::uint32_t> children;  ///< Indices of the child nodes.
        std::vector<std::uint32_t> meshes;    ///< Indices of the mesh entries this node draws.
    };

    /**
     * @brief One mesh of the model, together with the material it uses.
     */
    struct Entry {
        intrusive_ptr<Mesh>     mesh;     ///< The mesh; never null in a filled entry.
        intrusive_ptr<Material> material; ///< The material the mesh uses; null when the source declared none.
    };

  public:
    /**
     * @brief Constructs an empty model.
     */
    Model();

  public:
    /**
     * @brief Returns the index of the root node.
     *
     * @return The node index the tree starts at; 0 for a filled model.
     */
    [[nodiscard]]
    std::uint32_t root() const noexcept;

    /**
     * @brief Sets the index of the root node.
     *
     * @param index The node index the tree starts at.
     */
    void setRoot(std::uint32_t index) noexcept;

    /**
     * @brief Returns the nodes of the tree.
     *
     * @return The nodes, in the order the source listed them.
     */
    [[nodiscard]]
    const std::vector<Node>& nodes() const noexcept;

    /**
     * @brief Replaces the nodes.
     *
     * @param nodes The nodes; the root is the one setRoot() points at.
     */
    void setNodes(std::vector<Node> nodes);

    /**
     * @brief Returns the mesh entries.
     *
     * @return The entries, in the order the source listed the meshes.
     */
    [[nodiscard]]
    const std::vector<Entry>& meshes() const noexcept;

    /**
     * @brief Replaces the mesh entries.
     *
     * @param meshes The entries.
     */
    void setMeshes(std::vector<Entry> meshes);

  private:
    /// Index of the root node.
    std::uint32_t root_{ 0 };

    /// The nodes of the tree.
    std::vector<Node> nodes_;

    /// The mesh entries the nodes refer to.
    std::vector<Entry> meshes_;
};

VN_GEOMETRY_NS_END
