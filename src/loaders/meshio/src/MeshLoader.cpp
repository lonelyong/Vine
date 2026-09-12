#include <vine/meshio/MeshLoader.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <string>
#include <utility>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/math/Vector3.hpp>

V_MESHIO_NS_BEGIN

namespace
{

using Mesh                = vine::geometry::Mesh;
using IndexedTriangleMesh = vine::geometry::IndexedTriangleMesh;
using UInt32Array         = vine::geometry::UInt32Array;
using Vec3fArray          = vine::geometry::Vec3fArray;

/** @brief AABB diagonal length above which the source unit is millimeters. */
constexpr float kMmThreshold = 10.0f;

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
    const Vec3fArray& positions = mesh.positions();
    if (positions.empty()) {
        return;
    }

    vine::math::Vec3f min = positions.front();
    vine::math::Vec3f max = positions.front();
    for (const auto& v : positions) {
        min.x = std::min(min.x, v.x);
        min.y = std::min(min.y, v.y);
        min.z = std::min(min.z, v.z);
        max.x = std::max(max.x, v.x);
        max.y = std::max(max.y, v.y);
        max.z = std::max(max.z, v.z);
    }

    double scale_factor = 1.0;
    if (options.scale_mode == MeshLoader::ScaleMode::Auto) {
        const auto  diagonal = (max - min).length();
        const auto  unit     = diagonal > kMmThreshold ? MeshLoader::LengthUnit::Millimeter : MeshLoader::LengthUnit::Meter;
        scale_factor         = static_cast<double>(options.auto_scale_output_unit) / static_cast<double>(unit);
    } else if (options.scale_mode == MeshLoader::ScaleMode::Custom) {
        scale_factor = options.custom_scale_factor;
    }

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

vine::intrusive_ptr<Mesh> MeshLoader::load(const std::filesystem::path& file_path)
{
    std::error_code ec;
    if (file_path.empty() || !std::filesystem::is_regular_file(file_path, ec) || ec) {
        return {};
    }

    const vine::crypto::ByteSequenceFingerprint fingerprint(file_path);
    if (auto cached = cache_.get(fingerprint))
    {
        const auto& option_map = cached->option_shape_map;
        const auto  it         = option_map.find(options_);
        if (it != option_map.end())
        {
            return it->second;
        }
    }

    Assimp::Importer         importer;
    constexpr unsigned int  load_flags = aiProcess_Triangulate
        | aiProcess_JoinIdenticalVertices
        | aiProcess_FindInvalidData
        | aiProcess_ImproveCacheLocality
        | aiProcess_FixInfacingNormals
        | aiProcess_PreTransformVertices
        | aiProcess_OptimizeMeshes;

    const aiScene* scene = importer.ReadFile(reinterpret_cast<const char*>(file_path.u8string().data()), load_flags);
    if (!scene) {
        return {};
    }

    auto mesh = vine::make_intrusive<IndexedTriangleMesh>();
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

V_MESHIO_NS_END
