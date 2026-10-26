#include <vine/meshio/MeshLoader.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <vine/geometry/ColorMaterial.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/PbrMaterial.hpp>
#include <vine/geometry/PhongMaterial.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>

VN_MESHIO_NS_BEGIN

namespace
{

using Mesh                = vn::geometry::Mesh;
using IndexedTriangleMesh = vn::geometry::IndexedTriangleMesh;
using Material            = vn::geometry::Material;
using Model               = vn::geometry::Model;
using UInt32Array         = vn::geometry::UInt32Array;
using Vec2fArray          = vn::geometry::Vec2fArray;
using Vec3fArray          = vn::geometry::Vec3fArray;
using Vec4fArray          = vn::geometry::Vec4fArray;

/** @brief AABB diagonal length above which the source unit is millimeters. */
constexpr float kMmThreshold = 10.0f;

// Both lists carry aiProcess_GenNormals, and it is worth knowing why. A file may describe no normals at all (an OBJ
// without `vn`), or ones that are worse than none: many STL writers store a zero facet normal and expect the reader to
// derive it - assimp's STL loader passes those through, and aiProcess_FindInvalidData (also in these lists, running
// BEFORE aiProcess_GenNormals) rejects a zero-length normal and drops the whole array. Either way the mesh would arrive
// without normals, and assimp fills them only for meshes that have none, so a file that does describe normals keeps them.
// The price is vertex splitting (a per-face normal cannot be shared by the faces around a vertex), which leaves
// aiProcess_JoinIdenticalVertices - it runs later - less to merge. That is the right trade: the alternative is a document
// whose mesh carries no normals at all, and STL - the format this repository's file payloads use - is a triangle soup
// whose facets are meant to be flat anyway.

/** @brief Post-processing every load runs, whichever entry it came in through. */
constexpr unsigned int kLoadFlags = aiProcess_Triangulate
    | aiProcess_JoinIdenticalVertices
    | aiProcess_FindInvalidData
    | aiProcess_GenNormals
    | aiProcess_ImproveCacheLocality
    | aiProcess_FixInfacingNormals
    | aiProcess_PreTransformVertices
    | aiProcess_OptimizeMeshes;

/**
 * @brief Post-processing the model entry runs.
 *
 * What is missing is the point: aiProcess_PreTransformVertices would delete the node graph and bake every transform into
 * the vertices, and aiProcess_OptimizeMeshes would merge meshes and drop nodes - both would throw away the structure
 * this entry exists to keep.
 */
constexpr unsigned int kModelFlags = aiProcess_Triangulate
    | aiProcess_JoinIdenticalVertices
    | aiProcess_FindInvalidData
    | aiProcess_GenNormals
    | aiProcess_ImproveCacheLocality
    | aiProcess_FixInfacingNormals;

/** @brief Bytes the stream entry moves per read while it drains a stream. */
constexpr std::size_t kDrainChunk = 64U * 1024U;

/**
 * @brief Reads a whole stream into memory, the way the stream entry has to.
 *
 * @param in The stream to read; a failing stream ends the read.
 * @param out Receives the bytes read so far, appended to what is already there.
 * @return true when the stream was read to its end, false when it broke.
 */
bool drainStream(std::istream& in, std::vector<unsigned char>& out)
{
    std::array<char, kDrainChunk> chunk{};
    while (true) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            out.insert(out.end(), chunk.data(), chunk.data() + static_cast<std::size_t>(got));
        }
        if (got < static_cast<std::streamsize>(chunk.size())) {
            break; // the end, or a source that broke - gcount() stops the loop either way
        }
    }
    return !in.bad();
}

/**
 * @brief The vertex arrays one builder accumulates, and the facts a multi-mesh build has to remember.
 *
 * Every conversion from an assimp mesh to Vine vertex data goes through appendMeshAttributes(), so this is the ONE
 * shape "a mesh became vertex data" takes. The two flags are decisions a MERGE has to make and a single-mesh build
 * does not, which is why they belong to the target being filled rather than to the conversion:
 * - pad_missing_colors: a mesh that authors no colour carries OPAQUE WHITE, the multiplicative identity the shading
 *   applies - exactly what "no colour channel" means - so a merged entry looks like its parts drawn on their own.
 * - keep_texcoords: the merged entry answers "give me the triangles" and records none; a model entry keeps what the
 *   file held.
 */
struct MeshSink
{
    Vec3fArray  positions; ///< Vertices appended so far, in scene order.
    Vec3fArray  normals;   ///< Per-vertex normals, in the same order as @ref positions.
    Vec4fArray  colors;    ///< Per-vertex colours (linear RGBA), in the same order as @ref positions.
    Vec2fArray  texcoords; ///< Per-vertex texture coordinates, when @ref keep_texcoords is on.
    UInt32Array indices;   ///< Triangle corners, rebased onto @ref positions.

    bool pad_missing_colors{ false };    ///< White for a mesh that authors none (see the note above).
    bool keep_texcoords{ false };        ///< Whether texture coordinates are recorded at all.
    bool any_mesh_has_colors{ false };   ///< True once an appended mesh AUTHORED a colour.
    bool every_mesh_has_normals{ true }; ///< False once an appended mesh carried none.
};

/**
 * @brief What both builders share: the scene they read, and the ONE translation from an assimp mesh to Vine arrays.
 *
 * Private inheritance in both builders, deliberately: the base is a tool, not an interface, so nothing outside the two
 * builders can use it. The split the file follows is the one the two entries force - what BOTH need lives here
 * (protected), and what exists for one entry lives private in that entry's builder (materials and node building in
 * ModelBuilder, node walking and scaling in MergedMeshBuilder).
 */
class MeshBuilderBase
{
  protected:
    /**
     * @brief Binds the builder to the scene it reads.
     *
     * @param scene The parsed scene; it outlives the builder.
     */
    explicit MeshBuilderBase(const aiScene& scene) noexcept
      : scene_(&scene)
    {
    }

    /**
     * @brief Returns the scene being read.
     *
     * @return The parsed scene the builder was constructed with.
     */
    [[nodiscard]] const aiScene& scene() const noexcept { return *scene_; }

    /**
     * @brief Appends one assimp mesh's vertex data to @p sink.
     *
     * The ONE place an aiMesh becomes Vine arrays: positions, the authored normals, the authored (or padded) colours,
     * the texture coordinates when the sink wants them, and the triangle corners rebased onto the vertices the sink
     * already holds.
     *
     * @param source The assimp mesh to append; a mesh without positions contributes nothing.
     * @param sink   The arrays being filled; its flags decide padding and whether coordinates are kept.
     */
    static void appendMeshAttributes(const aiMesh& source, MeshSink& sink)
    {
        if (!source.HasPositions()) {
            return; // nothing to append: the corners of a mesh without vertices name nothing
        }

        const auto vertex_base = static_cast<std::uint32_t>(sink.positions.size());

        sink.positions.reserve(sink.positions.size() + source.mNumVertices);
        for (unsigned int i = 0; i < source.mNumVertices; ++i) {
            const aiVector3D& v = source.mVertices[i];
            sink.positions.emplace_back(v.x, v.y, v.z);
        }

        if (source.HasNormals()) {
            sink.normals.reserve(sink.normals.size() + source.mNumVertices);
            for (unsigned int i = 0; i < source.mNumVertices; ++i) {
                const aiVector3D& n = source.mNormals[i];
                sink.normals.emplace_back(n.x, n.y, n.z);
            }
        } else {
            sink.every_mesh_has_normals = false; // a zero normal is not "no normal" (see MeshSink)
        }

        if (source.HasVertexColors(0)) {
            sink.any_mesh_has_colors = true;
            sink.colors.reserve(sink.colors.size() + source.mNumVertices);
            for (unsigned int i = 0; i < source.mNumVertices; ++i) {
                const aiColor4D& c = source.mColors[0][i];
                sink.colors.emplace_back(c.r, c.g, c.b, c.a);
            }
        } else if (sink.pad_missing_colors) {
            // The vertices are already in the list, so the colour array has to stay parallel to it: white modulates
            // the albedo by nothing, which is what "this mesh authors no colour" means to the shading.
            sink.colors.insert(sink.colors.end(), source.mNumVertices, vn::math::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));
        }

        if (sink.keep_texcoords && source.HasTextureCoords(0)) {
            sink.texcoords.reserve(sink.texcoords.size() + source.mNumVertices);
            for (unsigned int i = 0; i < source.mNumVertices; ++i) {
                const aiVector3D& t = source.mTextureCoords[0][i];
                sink.texcoords.emplace_back(t.x, t.y);
            }
        }

        if (source.HasFaces()) {
            sink.indices.reserve(sink.indices.size() + static_cast<std::size_t>(source.mNumFaces) * 3u);
            for (unsigned int i = 0; i < source.mNumFaces; ++i) {
                const aiFace& face = source.mFaces[i];
                if (face.mNumIndices == 3) {
                    sink.indices.push_back(face.mIndices[0] + vertex_base);
                    sink.indices.push_back(face.mIndices[1] + vertex_base);
                    sink.indices.push_back(face.mIndices[2] + vertex_base);
                }
            }
        }
    }

    /**
     * @brief Converts a measured diagonal length into the factor the load options ask for.
     *
     * Both entries share this rule, so the same source is scaled the same way whichever one reads it.
     *
     * @param options The load options.
     * @param diagonal The AABB diagonal length of the source, in source units.
     * @return The factor the geometry has to be scaled by.
     */
    static double scaleFactorOf(const MeshLoader::Options& options, double diagonal)
    {
        if (options.scale_mode == MeshLoader::ScaleMode::Auto) {
            const auto unit = diagonal > kMmThreshold ? MeshLoader::LengthUnit::Millimeter : MeshLoader::LengthUnit::Meter;
            return static_cast<double>(options.auto_scale_output_unit) / static_cast<double>(unit);
        }
        if (options.scale_mode == MeshLoader::ScaleMode::Custom) {
            return options.custom_scale_factor;
        }
        return 1.0;
    }

    /**
     * @brief Copies an assimp node matrix into ours.
     *
     * Both sides multiply a matrix with a column vector and keep the translation in the fourth column, so the elements
     * travel one to one - no transpose is involved.
     *
     * @param source The assimp matrix.
     * @return The same transform in our layout.
     */
    static vn::math::Mat4d toMatrix(const aiMatrix4x4& source)
    {
        return vn::math::Mat4d(source.a1, source.a2, source.a3, source.a4, source.b1, source.b2, source.b3, source.b4,
                               source.c1, source.c2, source.c3, source.c4, source.d1, source.d2, source.d3, source.d4);
    }

    /**
     * @brief Converts an assimp colour to ours.
     *
     * @param color The assimp colour.
     * @return The same colour (linear RGBA: the loader passes the file's values through).
     */
    static vn::Colorf colorOf(const aiColor4D& color) { return vn::Colorf(color.r, color.g, color.b, color.a); }

  private:
    const aiScene* scene_{ nullptr }; ///< The parsed scene; borrowed for the builder's whole life.
};

/**
 * @brief Builds the flattened entry: every mesh in ONE vertex list, with the node transforms applied by assimp.
 *
 * aiProcess_PreTransformVertices has already baked the graph, so this builder walks the tree only to find the meshes
 * in scene order; materials and structure are what a caller of this entry asked to drop (see loadMerged).
 */
class MergedMeshBuilder final : private MeshBuilderBase
{
  public:
    /**
     * @brief Binds the builder to the scene and the options whose scale rule it applies.
     *
     * @param options The load options.
     * @param scene The parsed scene.
     */
    MergedMeshBuilder(const MeshLoader::Options& options, const aiScene& scene)
      : MeshBuilderBase(scene)
      , options_(options)
    {
        sink_.pad_missing_colors = true; // a merged entry has to fill in the meshes that author no colour
    }

    /**
     * @brief Merges the whole scene into one indexed triangle mesh.
     *
     * @return The merged mesh; its arrays are empty when the scene holds no usable mesh.
     */
    vn::intrusive_ptr<Mesh> build()
    {
        appendNode(*scene().mRootNode);

        auto mesh = vn::make_intrusive<IndexedTriangleMesh>();
        mesh->setPositions(std::move(sink_.positions));
        mesh->setIndices(std::move(sink_.indices));
        // A colour array exists only when some mesh AUTHORED one; the other vertices already carry the white that
        // means "no colour channel" (see MeshSink). A partial NORMAL array is dropped instead of invented: an absent
        // channel already means "derive them from the geometry" to the renderer.
        if (sink_.any_mesh_has_colors) {
            mesh->setColors(std::move(sink_.colors));
        }
        if (sink_.every_mesh_has_normals) {
            mesh->setNormals(std::move(sink_.normals));
        }
        applyScale(*mesh);
        return mesh;
    }

  private:
    /**
     * @brief Appends every mesh of @p node, and of its children, to the sink.
     *
     * @param node The node to walk; the transforms are already baked, so no matrix is threaded through.
     */
    void appendNode(const aiNode& node)
    {
        for (unsigned int i = 0; i < node.mNumMeshes; ++i) {
            const unsigned int index = node.mMeshes[i];
            if (index < scene().mNumMeshes) {
                MeshBuilderBase::appendMeshAttributes(*scene().mMeshes[index], sink_);
            }
        }
        for (unsigned int i = 0; i < node.mNumChildren; ++i) {
            appendNode(*node.mChildren[i]);
        }
    }

    /**
     * @brief Scales the merged vertices by the factor the load options state.
     *
     * @param mesh The mesh to scale in place; a mesh without vertices is left as it is.
     */
    void applyScale(IndexedTriangleMesh& mesh) const;

    const MeshLoader::Options& options_; ///< The load options the scale rule reads.
    MeshSink                   sink_;    ///< The arrays being merged.
};

void MergedMeshBuilder::applyScale(IndexedTriangleMesh& mesh) const
{
    // A borrowed view is enough here: the loop below reads it before setPositions() replaces the storage.
    const auto positions = mesh.positions();
    if (positions.empty()) {
        return;
    }

    vn::math::Vec3f min = positions.front();
    vn::math::Vec3f max = positions.front();
    for (const auto& v : positions) {
        min.x = std::min(min.x, v.x);
        min.y = std::min(min.y, v.y);
        min.z = std::min(min.z, v.z);
        max.x = std::max(max.x, v.x);
        max.y = std::max(max.y, v.y);
        max.z = std::max(max.z, v.z);
    }

    const double scale_factor = MeshBuilderBase::scaleFactorOf(options_, (max - min).length());
    if (scale_factor != 1.0) {
        Vec3fArray scaled;
        scaled.reserve(positions.size());
        for (const auto& v : positions) {
            scaled.emplace_back(static_cast<float>(v.x * scale_factor),
                                static_cast<float>(v.y * scale_factor),
                                static_cast<float>(v.z * scale_factor));
        }
        mesh.setPositions(std::move(scaled));
    }
}

/**
 * @brief Builds the structured entry: the node tree, one entry per mesh in its own local space, and the materials.
 *
 * Nothing is baked - a node's transform stays on the node and a mesh two nodes refer to stays one mesh - which is the
 * whole difference from MergedMeshBuilder. The model's scale is the same rule the merged entry uses, folded once into
 * the root's transform so the parts keep their places.
 */
class ModelBuilder final : private MeshBuilderBase
{
  public:
    /**
     * @brief Binds the builder to the scene and the options whose scale rule it applies.
     *
     * @param options The load options.
     * @param scene The parsed scene.
     */
    ModelBuilder(const MeshLoader::Options& options, const aiScene& scene)
      : MeshBuilderBase(scene)
      , options_(options)
    {
    }

    /**
     * @brief Builds the model from the whole scene.
     *
     * @return The model; it holds a root node and no entries when the scene carries no mesh.
     */
    vn::intrusive_ptr<Model> build();

  private:
    /**
     * @brief Reads one assimp material into the concrete material type that fits it.
     *
     * assimp describes every format's material through the same generic keys, so the type is chosen by what the source
     * actually provided rather than by the format it came from. A metallic/roughness pair asks for PbrMaterial, and so
     * does a material that is not fully opaque: only PbrMaterial carries opacity, so there the shading model is the part
     * that gives way. A specular color or a shininess is PhongMaterial, which is what an MTL describes. A plain diffuse
     * color is already a ColorMaterial. A material carries its own name, so nothing keeps a name table next to it.
     *
     * @param source The assimp material.
     * @return The material; never null.
     */
    static vn::intrusive_ptr<Material> makeMaterial(const aiMaterial& source);

    /**
     * @brief Builds one mesh from an assimp mesh, in the mesh's own space.
     *
     * The vertices are copied as they are: nothing is baked here, so a mesh two nodes refer to stays one mesh. Texture
     * coordinates and vertex colours are kept, because a caller that asked for the structure wants what the file held.
     *
     * @param source The assimp mesh.
     * @return The mesh.
     */
    vn::intrusive_ptr<Mesh> makeMesh(const aiMesh& source) const;

    /// Builds one entry per assimp mesh, sharing the material each mesh names.
    void collectEntries();

    /**
     * @brief Appends one assimp node and its children, children first.
     *
     * @param source The assimp node.
     * @param parent_world The transform of the parent chain; the node's own rides on top of it for measuring.
     * @return The index of the node that was added.
     */
    std::uint32_t appendNode(const aiNode& source, const vn::math::Mat4d& parent_world);

    /**
     * @brief Grows the model box by @p mesh placed at @p world.
     *
     * @param mesh The mesh to measure.
     * @param world The transform the vertices are placed by; the box is therefore in model space.
     */
    void growBounds(const Mesh& mesh, const vn::math::Mat4d& world);

    /// Folds the load options' scale into @p root's transform, once for the whole model.
    void applyScale(std::uint32_t root);

    const MeshLoader::Options& options_; ///< The load options the scale rule reads.
    std::vector<Model::Entry>   meshes_; ///< One entry per assimp mesh, in scene order.
    std::vector<Model::Node>    nodes_;  ///< The nodes built so far, children before parents.
    vn::math::Vec3f             min_{};  ///< Lower corner of the model box.
    vn::math::Vec3f             max_{};  ///< Upper corner of the model box.
    bool                        has_bounds_{ false }; ///< true once a vertex has been measured.
};

vn::intrusive_ptr<Material> ModelBuilder::makeMaterial(const aiMaterial& source)
{
    aiColor4D ambient{ 0.0f, 0.0f, 0.0f, 1.0f };
    aiColor4D diffuse{ 1.0f, 1.0f, 1.0f, 1.0f };
    aiColor4D specular{ 0.0f, 0.0f, 0.0f, 1.0f };
    float     shininess = 32.0f;
    float     opacity   = 1.0f;
    float     metallic  = 0.0f;
    float     roughness = 0.5f;

    source.Get(AI_MATKEY_COLOR_AMBIENT, ambient);
    source.Get(AI_MATKEY_COLOR_DIFFUSE, diffuse);
    const bool has_specular  = source.Get(AI_MATKEY_COLOR_SPECULAR, specular) == AI_SUCCESS;
    const bool has_shininess = source.Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS;
    const bool has_opacity   = source.Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS;
    const bool has_metallic  = source.Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS;
    const bool has_roughness = source.Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS;

    vn::intrusive_ptr<Material> material;
    if (has_metallic || has_roughness || (has_opacity && opacity < 1.0f)) {
        material = vn::make_intrusive<vn::geometry::PbrMaterial>(colorOf(diffuse), metallic, roughness, opacity);
    } else if (has_specular || has_shininess) {
        material = vn::make_intrusive<vn::geometry::PhongMaterial>(colorOf(ambient), colorOf(diffuse), colorOf(specular),
                                                                  shininess);
    } else {
        material = vn::make_intrusive<vn::geometry::ColorMaterial>(colorOf(diffuse));
    }

    aiString name;
    if (source.Get(AI_MATKEY_NAME, name) == AI_SUCCESS && name.length > 0) {
        material->setName(String::fromUtf8(std::string_view(name.C_Str(), name.length)));
    }
    return material;
}

vn::intrusive_ptr<Mesh> ModelBuilder::makeMesh(const aiMesh& source) const
{
    MeshSink sink;
    sink.keep_texcoords = true; // the structured entry keeps what the file held
    appendMeshAttributes(source, sink);

    auto mesh = vn::make_intrusive<IndexedTriangleMesh>();
    mesh->setPositions(std::move(sink.positions));
    mesh->setIndices(std::move(sink.indices));
    mesh->setTexcoords(std::move(sink.texcoords));
    // One mesh was appended, so "this mesh authored the attribute" is what the sink holds: an empty array is absent.
    if (!sink.normals.empty()) {
        mesh->setNormals(std::move(sink.normals));
    }
    if (!sink.colors.empty()) {
        mesh->setColors(std::move(sink.colors));
    }
    if (source.mName.length > 0) {
        mesh->setName(String::fromUtf8(std::string_view(source.mName.C_Str(), source.mName.length)));
    }
    return mesh;
}

void ModelBuilder::collectEntries()
{
    std::vector<vn::intrusive_ptr<Material>> materials;
    materials.reserve(scene().mNumMaterials);
    for (unsigned int i = 0; i < scene().mNumMaterials; ++i) {
        const aiMaterial* const source = scene().mMaterials[i];
        materials.push_back(source != nullptr ? makeMaterial(*source) : nullptr);
    }

    meshes_.reserve(scene().mNumMeshes);
    for (unsigned int i = 0; i < scene().mNumMeshes; ++i) {
        const aiMesh* const source = scene().mMeshes[i];
        Model::Entry        entry;
        if (source != nullptr) {
            entry.mesh = makeMesh(*source);
            if (source->mMaterialIndex < materials.size()) {
                entry.material = materials[source->mMaterialIndex];
            }
        }
        meshes_.push_back(std::move(entry));
    }
}

void ModelBuilder::growBounds(const Mesh& mesh, const vn::math::Mat4d& world)
{
    // Written out instead of a matrix-times-point call: the vertices are float and the transform is double, so the
    // helper would widen every vertex into a Point3d first - a copy per vertex in the one place a model is measured.
    for (const vn::math::Vec3f& point : mesh.positions()) {
        const auto x = static_cast<float>(world.element(0, 0) * point.x + world.element(0, 1) * point.y
                                          + world.element(0, 2) * point.z + world.element(0, 3));
        const auto y = static_cast<float>(world.element(1, 0) * point.x + world.element(1, 1) * point.y
                                          + world.element(1, 2) * point.z + world.element(1, 3));
        const auto z = static_cast<float>(world.element(2, 0) * point.x + world.element(2, 1) * point.y
                                          + world.element(2, 2) * point.z + world.element(2, 3));

        if (!has_bounds_) {
            min_        = vn::math::Vec3f(x, y, z);
            max_        = min_;
            has_bounds_ = true;
            continue;
        }
        min_.x = std::min(min_.x, x);
        min_.y = std::min(min_.y, y);
        min_.z = std::min(min_.z, z);
        max_.x = std::max(max_.x, x);
        max_.y = std::max(max_.y, y);
        max_.z = std::max(max_.z, z);
    }
}

std::uint32_t ModelBuilder::appendNode(const aiNode& source, const vn::math::Mat4d& parent_world)
{
    const vn::math::Mat4d local = toMatrix(source.mTransformation);
    const vn::math::Mat4d world = parent_world * local;

    std::vector<std::uint32_t> children;
    children.reserve(source.mNumChildren);
    for (unsigned int i = 0; i < source.mNumChildren; ++i) {
        children.push_back(appendNode(*source.mChildren[i], world));
    }

    std::vector<std::uint32_t> meshes;
    meshes.reserve(source.mNumMeshes);
    for (unsigned int i = 0; i < source.mNumMeshes; ++i) {
        const std::uint32_t index = source.mMeshes[i];
        if (index >= meshes_.size() || meshes_[index].mesh == nullptr) {
            continue; // an index the scene does not hold names nothing
        }
        meshes.push_back(index);
        growBounds(*meshes_[index].mesh, world);
    }

    Model::Node node;
    node.name      = source.mName.length > 0 ? String::fromUtf8(std::string_view(source.mName.C_Str(), source.mName.length))
                                            : String{};
    node.transform = local;
    node.children  = std::move(children);
    node.meshes    = std::move(meshes);

    nodes_.push_back(std::move(node));
    return static_cast<std::uint32_t>(nodes_.size() - 1);
}

void ModelBuilder::applyScale(std::uint32_t root)
{
    if (!has_bounds_) {
        return;
    }

    const double factor = MeshBuilderBase::scaleFactorOf(options_, (max_ - min_).length());
    if (factor == 1.0) {
        return;
    }

    nodes_[root].transform = vn::math::scale(factor) * nodes_[root].transform;
}

vn::intrusive_ptr<Model> ModelBuilder::build()
{
    collectEntries();

    const std::uint32_t root = appendNode(*scene().mRootNode, vn::math::Mat4d{});

    // One factor for the whole model, folded into the root's transform: a uniform scale applied there scales the vertices
    // and the offsets between nodes alike, so the parts stay where the source put them.
    applyScale(root);

    auto model = vn::make_intrusive<Model>();
    model->setRoot(root);
    model->setNodes(std::move(nodes_));
    model->setMeshes(std::move(meshes_));
    return model;
}

} // namespace

const std::set<String>& MeshLoader::supportedExtensions()
{
    // ASKED, not listed: the importer registry inside assimp is what decides whether a file can be opened, so a table
    // kept here was a second answer that went stale the first time a format was added - it was already missing .ply,
    // the very format the exporter beside this file writes. What comes back is assimp's own text form ("*.obj;*.stl"),
    // which is why each token loses its "*." before it becomes an extension here.
    static const std::set<String> extensions = [] {
        Assimp::Importer importer;
        std::string      listed;
        importer.GetExtensionList(listed);

        std::set<String> parsed;
        std::size_t      begin = 0;
        while (begin < listed.size()) {
            const std::size_t end   = listed.find_first_of("; ", begin);
            const std::string token = listed.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            begin                   = end == std::string::npos ? listed.size() : end + 1;

            const std::size_t star = token.find("*.");
            const std::string name = star == std::string::npos ? token : token.substr(star + 2);
            if (!name.empty()) {
                parsed.insert(String::fromUtf8("." + name).toLower());
            }
        }
        return parsed;
    }();
    return extensions;
}

MeshLoader::MeshLoader(Options options)
  : options_(std::move(options))
{
}

MeshLoader::~MeshLoader() = default;

MeshLoader& MeshLoader::defaultInstance()
{
    static MeshLoader instance;
    return instance;
}

bool MeshLoader::isSupportedFormat(const std::filesystem::path& file_path)
{
    if (!file_path.has_extension()) {
        return false;
    }

    // Compare in the same spelling the table uses (lowercase, leading dot): the bytes come from the path as they are,
    // and String::toLower() only folds the ASCII range, which is all an extension needs.
    return supportedExtensions().contains(String(file_path.extension().u8string()).toLower());
}

const MeshLoader::Options& MeshLoader::options() const noexcept
{
    return options_;
}

void MeshLoader::clearCache()
{
    cache_.clear();
}

vn::intrusive_ptr<Mesh> MeshLoader::loadMerged(const std::filesystem::path& file_path, vn::io::IoError* error)
{
    const auto failed = [error](vn::io::IoError reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return vn::intrusive_ptr<Mesh>{};
    };

    if (error != nullptr) {
        *error = vn::io::IoError::Ok;
    }

    std::error_code ec;
    if (file_path.empty() || !std::filesystem::is_regular_file(file_path, ec) || ec) {
        return failed(vn::io::IoError::NotFound);
    }

    vn::crypto::ByteSequenceFingerprint fingerprint;
    try {
        fingerprint = vn::crypto::ByteSequenceFingerprint(file_path);
    } catch (const std::exception&) {
        return failed(vn::io::IoError::IoFailure); // a file this process may not read is not geometry either
    }

    if (auto cached = cache_.get(fingerprint); cached && cached->merged) {
        return cached->merged;
    }

    Assimp::Importer     importer;
    const aiScene* const scene = importer.ReadFile(reinterpret_cast<const char*>(file_path.u8string().data()), kLoadFlags);
    if (scene == nullptr) {
        return failed(vn::io::IoError::InvalidData); // the bytes are not a model this build can parse
    }

    const auto mesh = MergedMeshBuilder(options_, *scene).build();

    auto cached   = cache_.get(fingerprint).value_or(Cached{});
    cached.merged = mesh;
    cache_.set(fingerprint, std::move(cached));

    return mesh;
}

vn::intrusive_ptr<Mesh> MeshLoader::loadMerged(std::istream& in, const char* format_hint, vn::io::IoError* error)
{
    std::vector<unsigned char> bytes;
    const bool                 read = drainStream(in, bytes);
    if (error != nullptr) {
        *error = vn::io::IoError::Ok;
    }
    if (!read) {
        if (error != nullptr) {
            *error = vn::io::IoError::IoFailure;
        }
        return {}; // a stream that broke is not content assimp should be asked to read
    }
    return loadMergedBytes(bytes, format_hint, error);
}

vn::intrusive_ptr<Mesh> MeshLoader::loadMergedBytes(std::vector<unsigned char>& bytes, const char* format_hint,
                                                   vn::io::IoError* error)
{
    const auto failed = [error](vn::io::IoError reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return vn::intrusive_ptr<Mesh>{};
    };

    if (error != nullptr) {
        *error = vn::io::IoError::Ok;
    }

    if (bytes.empty()) {
        return failed(vn::io::IoError::InvalidData);
    }
    const char* const hint = format_hint != nullptr ? format_hint : "";

    // The same content loaded through either entry hashes to the same fingerprint, so it is parsed once and reused.
    const vn::crypto::ByteSequenceFingerprint fingerprint(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    if (auto cached = cache_.get(fingerprint); cached && cached->merged) {
        return cached->merged;
    }

    // assimp parses memory rather than a stream, which is why the caller had to hand over an image in the first place.
    Assimp::Importer importer;
    const aiScene*   scene = importer.ReadFileFromMemory(bytes.data(), bytes.size(), kLoadFlags, hint);
    if (scene == nullptr) {
        return failed(vn::io::IoError::InvalidData);
    }

    const auto mesh = MergedMeshBuilder(options_, *scene).build();

    auto cached   = cache_.get(fingerprint).value_or(Cached{});
    cached.merged = mesh;
    cache_.set(fingerprint, std::move(cached));

    return mesh;
}

vn::intrusive_ptr<Model> MeshLoader::load(const std::filesystem::path& file_path, vn::io::IoError* error)
{
    const auto failed = [error](vn::io::IoError reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return vn::intrusive_ptr<Model>{};
    };

    if (error != nullptr) {
        *error = vn::io::IoError::Ok;
    }

    std::error_code ec;
    if (file_path.empty() || !std::filesystem::is_regular_file(file_path, ec) || ec) {
        return failed(vn::io::IoError::NotFound);
    }

    vn::crypto::ByteSequenceFingerprint fingerprint;
    try {
        fingerprint = vn::crypto::ByteSequenceFingerprint(file_path);
    } catch (const std::exception&) {
        return failed(vn::io::IoError::IoFailure);
    }

    if (auto cached = cache_.get(fingerprint); cached && cached->model) {
        return cached->model;
    }

    Assimp::Importer     importer;
    const aiScene* const scene = importer.ReadFile(reinterpret_cast<const char*>(file_path.u8string().data()), kModelFlags);
    if (scene == nullptr) {
        return failed(vn::io::IoError::InvalidData);
    }
    return cacheModel(fingerprint, ModelBuilder(options_, *scene).build());
}

vn::intrusive_ptr<Model> MeshLoader::load(std::istream& in, const char* format_hint, vn::io::IoError* error)
{
    std::vector<unsigned char> bytes;
    const bool                 read = drainStream(in, bytes);
    if (error != nullptr) {
        *error = vn::io::IoError::Ok;
    }
    if (!read) {
        if (error != nullptr) {
            *error = vn::io::IoError::IoFailure;
        }
        return {}; // a stream that broke is not content assimp should be asked to read
    }
    return loadBytes(bytes, format_hint, error);
}

vn::intrusive_ptr<Model> MeshLoader::loadBytes(std::vector<unsigned char>& bytes, const char* format_hint,
                                               vn::io::IoError* error)
{
    const auto failed = [error](vn::io::IoError reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return vn::intrusive_ptr<Model>{};
    };

    if (error != nullptr) {
        *error = vn::io::IoError::Ok;
    }

    if (bytes.empty()) {
        return failed(vn::io::IoError::InvalidData);
    }
    const char* const hint = format_hint != nullptr ? format_hint : "";

    // The same content loaded through either entry hashes to the same fingerprint, so it is parsed once per entry.
    const vn::crypto::ByteSequenceFingerprint fingerprint(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    if (auto cached = cache_.get(fingerprint); cached && cached->model) {
        return cached->model;
    }

    // assimp parses memory rather than a stream, which is why the caller had to hand over an image in the first place.
    Assimp::Importer importer;
    const aiScene*   scene = importer.ReadFileFromMemory(bytes.data(), bytes.size(), kModelFlags, hint);
    if (scene == nullptr) {
        return failed(vn::io::IoError::InvalidData);
    }
    return cacheModel(fingerprint, ModelBuilder(options_, *scene).build());
}

vn::intrusive_ptr<Model> MeshLoader::cacheModel(const vn::crypto::ByteSequenceFingerprint& fingerprint,
                                                vn::intrusive_ptr<Model>                      model)
{
    if (fingerprint) {
        auto cached  = cache_.get(fingerprint).value_or(Cached{});
        cached.model = model;
        cache_.set(fingerprint, std::move(cached));
    }
    return model;
}

VN_MESHIO_NS_END
