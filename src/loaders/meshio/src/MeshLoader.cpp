#include <vine/meshio/MeshLoader.hpp>

#include <algorithm>
#include <array>
#include <cctype>
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

/** @brief AABB diagonal length above which the source unit is millimeters. */
constexpr float kMmThreshold = 10.0f;

/** @brief Post-processing every load runs, whichever entry it came in through. */
constexpr unsigned int kLoadFlags = aiProcess_Triangulate
    | aiProcess_JoinIdenticalVertices
    | aiProcess_FindInvalidData
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
 * @brief Returns the set of supported mesh file extensions.
 *
 * @return The extension set.
 */
const std::set<std::string>& supportedExtensions()
{
    static const std::set<std::string> extensions = {
        ".stl", ".obj", ".gltf", ".3mf", ".3ds", ".dxf",
        ".ifc", ".ac", ".ac3d", ".lxo", ".fbx", ".dae",
    };
    return extensions;
}

/**
 * @brief Recursively collects every mesh of an assimp scene.
 *
 * All meshes are merged into single arrays; indices are rebased so they stay
 * valid across the concatenated vertex list.
 *
 * @param scene The assimp scene.
 * @param node The current scene node.
 * @param positions Output vertex positions.
 * @param normals Output vertex normals.
 * @param indices Output triangle indices.
 */
void collectAssimpNode(const aiScene* scene,
                       const aiNode* node,
                       Vec3fArray& positions,
                       Vec3fArray& normals,
                       UInt32Array& indices)
{
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        const aiMesh*      ai_mesh     = scene->mMeshes[node->mMeshes[i]];
        const std::uint32_t vertex_base = static_cast<std::uint32_t>(positions.size());

        if (ai_mesh->HasPositions()) {
            positions.reserve(positions.size() + ai_mesh->mNumVertices);
            for (unsigned int j = 0; j < ai_mesh->mNumVertices; ++j) {
                const aiVector3D& v = ai_mesh->mVertices[j];
                positions.emplace_back(v.x, v.y, v.z);
            }
        }

        if (ai_mesh->HasNormals()) {
            normals.reserve(normals.size() + ai_mesh->mNumVertices);
            for (unsigned int j = 0; j < ai_mesh->mNumVertices; ++j) {
                const aiVector3D& n = ai_mesh->mNormals[j];
                normals.emplace_back(n.x, n.y, n.z);
            }
        }

        if (ai_mesh->HasFaces()) {
            indices.reserve(indices.size() + ai_mesh->mNumFaces * 3);
            for (unsigned int j = 0; j < ai_mesh->mNumFaces; ++j) {
                const aiFace& face = ai_mesh->mFaces[j];
                if (face.mNumIndices == 3) {
                    indices.push_back(face.mIndices[0] + vertex_base);
                    indices.push_back(face.mIndices[1] + vertex_base);
                    indices.push_back(face.mIndices[2] + vertex_base);
                }
            }
        }
    }

    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        collectAssimpNode(scene, node->mChildren[i], positions, normals, indices);
    }
}

/**
 * @brief Merges an assimp scene into one indexed triangle mesh.
 *
 * @param mesh The target mesh.
 * @param scene The assimp scene.
 */
void mergeAssimpScene(IndexedTriangleMesh& mesh, const aiScene* scene)
{
    Vec3fArray  positions;
    Vec3fArray  normals;
    UInt32Array indices;

    collectAssimpNode(scene, scene->mRootNode, positions, normals, indices);

    mesh.setPositions(std::move(positions));
    mesh.setNormals(std::move(normals));
    mesh.setTexcoords({});
    mesh.setIndices(std::move(indices));
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
double scaleFactorOf(const MeshLoader::Options& options, double diagonal)
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
 * @brief Scales the vertices of a mesh according to the load options.
 *
 * Auto mode infers the source unit from the AABB diagonal length: a diagonal
 * above kMmThreshold is treated as millimeters, otherwise as meters. The
 * vertices are then converted to the configured output unit.
 *
 * @param options The load options.
 * @param mesh The mesh to scale.
 */
void applyScale(const MeshLoader::Options& options, IndexedTriangleMesh& mesh)
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

    const double scale_factor = scaleFactorOf(options, (max - min).length());

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
 * @brief Copies an assimp node matrix into ours.
 *
 * Both sides multiply a matrix with a column vector and keep the translation in the fourth column, so the elements travel
 * one to one - no transpose is involved.
 *
 * @param source The assimp matrix.
 * @return The same transform in our layout.
 */
vn::math::Mat4d toMatrix(const aiMatrix4x4& source)
{
    return vn::math::Mat4d(source.a1, source.a2, source.a3, source.a4, source.b1, source.b2, source.b3, source.b4,
                           source.c1, source.c2, source.c3, source.c4, source.d1, source.d2, source.d3, source.d4);
}

/**
 * @brief Builds the matrix that scales every axis by one factor.
 *
 * @param factor The scale factor.
 * @return The uniform scale matrix.
 */
vn::math::Mat4d uniformScale(double factor)
{
    return vn::math::Mat4d(factor, 0.0, 0.0, 0.0, 0.0, factor, 0.0, 0.0, 0.0, 0.0, factor, 0.0, 0.0, 0.0, 0.0, 1.0);
}

/**
 * @brief Converts an assimp color to ours.
 *
 * @param color The assimp color.
 * @return The color.
 */
vn::Colorf colorOf(const aiColor4D& color)
{
    return vn::Colorf(color.r, color.g, color.b, color.a);
}

/**
 * @brief Reads one assimp material into the concrete material type that fits it.
 *
 * assimp describes every format's material through the same generic keys, so the type is chosen by what the source
 * actually provided rather than by the format it came from. A metallic/roughness pair asks for PbrMaterial, and so does a
 * material that is not fully opaque: only PbrMaterial carries opacity, so there the shading model is the part that gives
 * way. A specular color or a shininess is PhongMaterial, which is what an MTL describes. A plain diffuse color is already
 * a ColorMaterial. A material carries its own name, so nothing keeps a name table next to it.
 *
 * @param source The assimp material.
 * @return The material; never null.
 */
vn::intrusive_ptr<Material> makeMaterial(const aiMaterial& source)
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

/**
 * @brief Builds one mesh from an assimp mesh, in the mesh's own space.
 *
 * The vertices are copied as they are: nothing is baked here, so a mesh two nodes refer to stays one mesh. Texture
 * coordinates are kept, because a caller that asked for the structure wants what the file held.
 *
 * @param source The assimp mesh.
 * @return The mesh.
 */
vn::intrusive_ptr<Mesh> makeMesh(const aiMesh& source)
{
    Vec3fArray  positions;
    Vec3fArray  normals;
    Vec2fArray  texcoords;
    UInt32Array indices;

    if (source.HasPositions()) {
        positions.reserve(source.mNumVertices);
        for (unsigned int i = 0; i < source.mNumVertices; ++i) {
            const aiVector3D& v = source.mVertices[i];
            positions.emplace_back(v.x, v.y, v.z);
        }
    }
    if (source.HasNormals()) {
        normals.reserve(source.mNumVertices);
        for (unsigned int i = 0; i < source.mNumVertices; ++i) {
            const aiVector3D& n = source.mNormals[i];
            normals.emplace_back(n.x, n.y, n.z);
        }
    }
    if (source.HasTextureCoords(0)) {
        texcoords.reserve(source.mNumVertices);
        for (unsigned int i = 0; i < source.mNumVertices; ++i) {
            const aiVector3D& t = source.mTextureCoords[0][i];
            texcoords.emplace_back(t.x, t.y);
        }
    }
    if (source.HasFaces()) {
        indices.reserve(source.mNumFaces * 3);
        for (unsigned int i = 0; i < source.mNumFaces; ++i) {
            const aiFace& face = source.mFaces[i];
            if (face.mNumIndices == 3) {
                indices.push_back(face.mIndices[0]);
                indices.push_back(face.mIndices[1]);
                indices.push_back(face.mIndices[2]);
            }
        }
    }

    auto mesh = vn::make_intrusive<IndexedTriangleMesh>();
    mesh->setPositions(std::move(positions));
    mesh->setNormals(std::move(normals));
    mesh->setTexcoords(std::move(texcoords));
    mesh->setIndices(std::move(indices));
    if (source.mName.length > 0) {
        mesh->setName(String::fromUtf8(std::string_view(source.mName.C_Str(), source.mName.length)));
    }
    return mesh;
}

/**
 * @brief The state one model build carries while the assimp scene is walked.
 */
struct ModelBuilder
{
    std::vector<vn::intrusive_ptr<Material>> materials;         ///< One material per assimp material, shared by its meshes.
    std::vector<Model::Entry>                meshes;            ///< One entry per assimp mesh, in scene order.
    std::vector<Model::Node>                 nodes;             ///< The nodes built so far, children before parents.
    vn::math::Vec3f                          min{};             ///< Lower corner of the model box.
    vn::math::Vec3f                          max{};             ///< Upper corner of the model box.
    bool                                     has_bounds{ false }; ///< true once a vertex has been measured.
};

/**
 * @brief Grows a model's bounding box by a mesh placed at a transform.
 *
 * @param builder The model being built; its box is grown.
 * @param mesh The mesh to measure.
 * @param world The transform the vertices are placed by; the box is therefore in model space.
 */
void growBounds(ModelBuilder& builder, const Mesh& mesh, const vn::math::Mat4d& world)
{
    for (const vn::math::Vec3f& point : mesh.positions()) {
        const auto x = static_cast<float>(world.element(0, 0) * point.x + world.element(0, 1) * point.y
                                          + world.element(0, 2) * point.z + world.element(0, 3));
        const auto y = static_cast<float>(world.element(1, 0) * point.x + world.element(1, 1) * point.y
                                          + world.element(1, 2) * point.z + world.element(1, 3));
        const auto z = static_cast<float>(world.element(2, 0) * point.x + world.element(2, 1) * point.y
                                          + world.element(2, 2) * point.z + world.element(2, 3));

        if (!builder.has_bounds) {
            builder.min        = vn::math::Vec3f(x, y, z);
            builder.max        = builder.min;
            builder.has_bounds = true;
            continue;
        }
        builder.min.x = std::min(builder.min.x, x);
        builder.min.y = std::min(builder.min.y, y);
        builder.min.z = std::min(builder.min.z, z);
        builder.max.x = std::max(builder.max.x, x);
        builder.max.y = std::max(builder.max.y, y);
        builder.max.z = std::max(builder.max.z, z);
    }
}

/**
 * @brief Turns one assimp node into a model node, its children first.
 *
 * The children come first because a node refers to them by index, so the vector grows bottom up: the root ends up as the
 * last node, and that index is what the model's root is set to.
 *
 * @param builder The model being built.
 * @param source The assimp node.
 * @param parent_world The transform of the parent chain; the node's own transform rides on top of it for measuring.
 * @return The index of the node that was added.
 */
std::uint32_t buildNode(ModelBuilder& builder, const aiNode& source, const vn::math::Mat4d& parent_world)
{
    const vn::math::Mat4d local = toMatrix(source.mTransformation);
    const vn::math::Mat4d world = parent_world * local;

    std::vector<std::uint32_t> children;
    children.reserve(source.mNumChildren);
    for (unsigned int i = 0; i < source.mNumChildren; ++i) {
        children.push_back(buildNode(builder, *source.mChildren[i], world));
    }

    std::vector<std::uint32_t> meshes;
    meshes.reserve(source.mNumMeshes);
    for (unsigned int i = 0; i < source.mNumMeshes; ++i) {
        const std::uint32_t index = source.mMeshes[i];
        if (index >= builder.meshes.size()) {
            continue; // an index the scene does not hold names nothing
        }
        meshes.push_back(index);
        growBounds(builder, *builder.meshes[index].mesh, world);
    }

    Model::Node node;
    node.name      = source.mName.length > 0 ? String::fromUtf8(std::string_view(source.mName.C_Str(), source.mName.length))
                                            : String{};
    node.transform = local;
    node.children  = std::move(children);
    node.meshes    = std::move(meshes);

    builder.nodes.push_back(std::move(node));
    return static_cast<std::uint32_t>(builder.nodes.size() - 1);
}

/**
 * @brief Builds a model from a parsed assimp scene.
 *
 * @param scene The parsed scene.
 * @param options The load options.
 * @return The model.
 */
vn::intrusive_ptr<Model> buildModel(const aiScene& scene, const MeshLoader::Options& options)
{
    ModelBuilder builder;

    builder.materials.reserve(scene.mNumMaterials);
    for (unsigned int i = 0; i < scene.mNumMaterials; ++i) {
        builder.materials.push_back(makeMaterial(*scene.mMaterials[i]));
    }

    builder.meshes.reserve(scene.mNumMeshes);
    for (unsigned int i = 0; i < scene.mNumMeshes; ++i) {
        const aiMesh& source = *scene.mMeshes[i];
        Model::Entry  entry;
        entry.mesh = makeMesh(source);
        if (source.mMaterialIndex < builder.materials.size()) {
            entry.material = builder.materials[source.mMaterialIndex];
        }
        builder.meshes.push_back(std::move(entry));
    }

    const std::uint32_t root = buildNode(builder, *scene.mRootNode, vn::math::Mat4d{});

    // One factor for the whole model, folded into the root's transform: a uniform scale applied there scales the vertices
    // and the offsets between nodes alike, so the parts stay where the source put them.
    if (builder.has_bounds) {
        const double factor = scaleFactorOf(options, (builder.max - builder.min).length());
        if (factor != 1.0) {
            builder.nodes[root].transform = uniformScale(factor) * builder.nodes[root].transform;
        }
    }

    auto model = vn::make_intrusive<Model>();
    model->setRoot(root);
    model->setNodes(std::move(builder.nodes));
    model->setMeshes(std::move(builder.meshes));
    return model;
}

} // namespace

MeshLoader::MeshLoader() = default;

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

    std::string extension = file_path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return supportedExtensions().contains(extension);
}

MeshLoader::Options& MeshLoader::options() noexcept
{
    return options_;
}

void MeshLoader::setOptions(const Options& options)
{
    options_ = options;
}

vn::intrusive_ptr<Mesh> MeshLoader::load(const std::filesystem::path& file_path)
{
    std::error_code ec;
    if (file_path.empty() || !std::filesystem::is_regular_file(file_path, ec) || ec) {
        return {};
    }

    const vn::crypto::ByteSequenceFingerprint fingerprint(file_path);
    if (auto cached = cache_.get(fingerprint))
    {
        const auto& option_map = cached->option_shape_map;
        const auto  it         = option_map.find(options_);
        if (it != option_map.end())
        {
            return it->second;
        }
    }

    Assimp::Importer     importer;
    const aiScene* const scene = importer.ReadFile(reinterpret_cast<const char*>(file_path.u8string().data()), kLoadFlags);
    if (!scene) {
        return {};
    }

    auto mesh = vn::make_intrusive<IndexedTriangleMesh>();
    mergeAssimpScene(*mesh, scene);
    applyScale(options_, *mesh);

    if (fingerprint)
    {
        auto cached = cache_.get(fingerprint).value_or(CacheData{});
        cached.option_shape_map.insert_or_assign(options_, mesh);
        cache_.set(fingerprint, std::move(cached), -1);
    }

    return mesh;
}

vn::intrusive_ptr<Mesh> MeshLoader::load(std::istream& in, const char* format_hint)
{
    std::vector<unsigned char> bytes;
    if (!drainStream(in, bytes)) {
        return {}; // a stream that broke is not content assimp should be asked to read
    }
    return loadBytes(bytes, format_hint);
}

vn::intrusive_ptr<Mesh> MeshLoader::loadBytes(std::vector<unsigned char>& bytes, const char* format_hint)
{
    if (bytes.empty()) {
        return {};
    }
    const char* const hint = format_hint != nullptr ? format_hint : "";

    // The same content loaded through either entry hashes to the same fingerprint, so it is parsed once and reused.
    const vn::crypto::ByteSequenceFingerprint fingerprint(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    if (auto cached = cache_.get(fingerprint)) {
        const auto& option_map = cached->option_shape_map;
        const auto  it         = option_map.find(options_);
        if (it != option_map.end()) {
            return it->second;
        }
    }

    // assimp parses memory rather than a stream, which is why the caller had to hand over an image in the first place.
    Assimp::Importer    importer;
    const aiScene*      scene = importer.ReadFileFromMemory(bytes.data(), bytes.size(), kLoadFlags, hint);
    if (scene == nullptr) {
        return {};
    }

    auto mesh = vn::make_intrusive<IndexedTriangleMesh>();
    mergeAssimpScene(*mesh, scene);
    applyScale(options_, *mesh);

    auto cached = cache_.get(fingerprint).value_or(CacheData{});
    cached.option_shape_map.insert_or_assign(options_, mesh);
    cache_.set(fingerprint, std::move(cached), -1);

    return mesh;
}

vn::intrusive_ptr<Model> MeshLoader::loadModel(const std::filesystem::path& file_path)
{
    std::error_code ec;
    if (file_path.empty() || !std::filesystem::is_regular_file(file_path, ec) || ec) {
        return {};
    }

    const vn::crypto::ByteSequenceFingerprint fingerprint(file_path);
    if (auto cached = cache_.get(fingerprint)) {
        const auto& option_map = cached->option_model_map;
        const auto  it         = option_map.find(options_);
        if (it != option_map.end()) {
            return it->second;
        }
    }

    Assimp::Importer     importer;
    const aiScene* const scene = importer.ReadFile(reinterpret_cast<const char*>(file_path.u8string().data()), kModelFlags);
    if (!scene) {
        return {};
    }
    return rememberModel(fingerprint, buildModel(*scene, options_));
}

vn::intrusive_ptr<Model> MeshLoader::loadModel(std::istream& in, const char* format_hint)
{
    std::vector<unsigned char> bytes;
    if (!drainStream(in, bytes)) {
        return {}; // a stream that broke is not content assimp should be asked to read
    }
    return loadModelBytes(bytes, format_hint);
}

vn::intrusive_ptr<Model> MeshLoader::loadModelBytes(std::vector<unsigned char>& bytes, const char* format_hint)
{
    if (bytes.empty()) {
        return {};
    }
    const char* const hint = format_hint != nullptr ? format_hint : "";

    // The same content loaded through either entry hashes to the same fingerprint, so it is parsed once per entry.
    const vn::crypto::ByteSequenceFingerprint fingerprint(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    if (auto cached = cache_.get(fingerprint)) {
        const auto& option_map = cached->option_model_map;
        const auto  it         = option_map.find(options_);
        if (it != option_map.end()) {
            return it->second;
        }
    }

    // assimp parses memory rather than a stream, which is why the caller had to hand over an image in the first place.
    Assimp::Importer    importer;
    const aiScene*      scene = importer.ReadFileFromMemory(bytes.data(), bytes.size(), kModelFlags, hint);
    if (scene == nullptr) {
        return {};
    }
    return rememberModel(fingerprint, buildModel(*scene, options_));
}

vn::intrusive_ptr<Model> MeshLoader::rememberModel(const vn::crypto::ByteSequenceFingerprint& fingerprint,
                                                   vn::intrusive_ptr<Model>                      model)
{
    if (fingerprint) {
        auto cached = cache_.get(fingerprint).value_or(CacheData{});
        cached.option_model_map.insert_or_assign(options_, model);
        cache_.set(fingerprint, std::move(cached), -1);
    }
    return model;
}

VN_MESHIO_NS_END
