#include <vine/meshio/MeshExporter.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>

#include <assimp/Exporter.hpp>
#include <assimp/scene.h>

#include <vine/Object.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/TriangleMesh.hpp>

V_MESHIO_NS_BEGIN

namespace
{

using Mesh                = vine::geometry::Mesh;
using TriangleMesh        = vine::geometry::TriangleMesh;
using IndexedTriangleMesh = vine::geometry::IndexedTriangleMesh;
using Vec3f               = vine::math::Vec3f;

/**
 * @brief RAII owner for a manually built assimp scene.
 *
 * The aiScene destructor releases every sub-object recursively (root node,
 * meshes, faces, materials), so exporting can throw without leaking and the
 * guard needs no manual cleanup.
 */
class AiSceneGuard
{
  public:
    /**
     * @brief Returns the managed scene.
     *
     * @return The scene pointer.
     */
    aiScene* get() noexcept
    {
        return &scene_;
    }

  private:
    aiScene scene_{};
};

/**
 * @brief View over the geometry of a triangle mesh, independent of whether it
 *        is stored as an indexed or a non-indexed mesh.
 *
 * An empty `indices` view means "non-indexed": a scene is only built for a mesh that passed `isValid()`,
 * and a valid indexed mesh always holds at least one triangle.
 */
struct MeshData
{
    /// Vertex positions; never empty for a valid triangle mesh.
    std::span<const Vec3f> positions;
    /// Optional per-vertex normals (empty when absent).
    std::span<const Vec3f> normals;
    /// Triangle indices; empty for a non-indexed mesh.
    std::span<const std::uint32_t> indices;
};

/**
 * @brief Builds an assimp scene from a Vine triangle mesh.
 *
 * @param guard Owns the built scene.
 * @param mesh The source mesh.
 * @param options The export options (scale factor).
 * @throws std::invalid_argument when the mesh is invalid.
 * @throws std::runtime_error when the mesh type is unsupported.
 */
void buildAiScene(AiSceneGuard& guard, const Mesh& mesh, const MeshExporter::Options& options)
{
    if (!mesh.isValid()) {
        throw std::invalid_argument("buildAiScene: mesh is invalid");
    }

    MeshData data;
    switch (mesh.shapeType()) {
      case vine::geometry::ShapeType::IndexedTriangleMesh: {
          const auto& itm = obj_cast<IndexedTriangleMesh>(mesh);
          data            = { itm.positions(), itm.normals(), itm.indices() };
          break;
      }
      case vine::geometry::ShapeType::TriangleMesh: {
          const auto& tm = obj_cast<TriangleMesh>(mesh);
          data           = { tm.positions(), tm.normals(), {} };
          break;
      }
      default:
          throw std::runtime_error("buildAiScene: unsupported mesh type");
    }

    aiScene& ai_scene            = *guard.get();
    ai_scene.mNumMeshes          = 1;
    ai_scene.mMeshes             = new aiMesh*[1]{ new aiMesh{} };
    ai_scene.mRootNode           = new aiNode{};
    ai_scene.mRootNode->mNumMeshes = 1;
    ai_scene.mRootNode->mMeshes    = new unsigned int[1]{ 0 };
    ai_scene.mNumMaterials       = 1;
    ai_scene.mMaterials          = new aiMaterial*[1]{ new aiMaterial() };

    aiMesh&       ai_mesh = *ai_scene.mMeshes[0];
    const float   scale   = static_cast<float>(options.scale_factor);

    if (!data.positions.empty()) {
        ai_mesh.mNumVertices = static_cast<unsigned int>(data.positions.size());
        ai_mesh.mVertices    = new aiVector3D[ai_mesh.mNumVertices];
        for (unsigned int i = 0; i < ai_mesh.mNumVertices; ++i) {
            const auto& v = data.positions[i];
            ai_mesh.mVertices[i] = aiVector3D(v.x * scale, v.y * scale, v.z * scale);
        }

        if (data.normals.size() == data.positions.size()) {
            ai_mesh.mNormals = new aiVector3D[ai_mesh.mNumVertices];
            for (unsigned int i = 0; i < ai_mesh.mNumVertices; ++i) {
                const auto& n = data.normals[i];
                ai_mesh.mNormals[i] = aiVector3D(n.x, n.y, n.z);
            }
        }
    }

    const std::size_t triangle_count = data.indices.empty() ? data.positions.size() / 3 : data.indices.size() / 3;
    ai_mesh.mNumFaces               = static_cast<unsigned int>(triangle_count);
    ai_mesh.mFaces                  = new aiFace[ai_mesh.mNumFaces];
    for (unsigned int i = 0; i < ai_mesh.mNumFaces; ++i) {
        ai_mesh.mFaces[i].mNumIndices = 3;
        if (!data.indices.empty()) {
            const std::size_t base = static_cast<std::size_t>(i) * 3;
            ai_mesh.mFaces[i].mIndices = new unsigned int[3]{ data.indices[base],
                                                              data.indices[base + 1],
                                                              data.indices[base + 2] };
        } else {
            const unsigned int base = i * 3;
            ai_mesh.mFaces[i].mIndices = new unsigned int[3]{ base, base + 1, base + 2 };
        }
    }
}

} // namespace

MeshExporter::MeshExporter() = default;

MeshExporter::~MeshExporter() = default;

MeshExporter& MeshExporter::defaultInstance()
{
    static MeshExporter instance;
    return instance;
}

MeshExporter::Options& MeshExporter::options() noexcept
{
    return options_;
}

void MeshExporter::setOptions(const Options& options)
{
    options_ = options;
}

void MeshExporter::exportAsStl(const Mesh& mesh, const std::filesystem::path& file_path) const
{
    AiSceneGuard guard;
    buildAiScene(guard, mesh, options_);

    if (file_path.has_parent_path()) {
        std::filesystem::create_directories(file_path.parent_path());
    }

    Assimp::Exporter          exporter;
    Assimp::ExportProperties  props;
    const std::string         fmt = options_.format == Options::Format::Ascii ? "stl" : "stlb";

    const aiReturn status = exporter.Export(guard.get(), fmt.c_str(),
                                            reinterpret_cast<const char*>(file_path.u8string().data()), 0, &props);
    if (status != AI_SUCCESS) {
        throw std::runtime_error("MeshExporter::exportAsStl: failed to export mesh to stl, " + std::string(exporter.GetErrorString()));
    }
}

void MeshExporter::exportAsObj(const Mesh& mesh, const std::filesystem::path& file_path) const
{
    AiSceneGuard guard;
    buildAiScene(guard, mesh, options_);

    if (file_path.has_parent_path()) {
        std::filesystem::create_directories(file_path.parent_path());
    }

    Assimp::Exporter         exporter;
    Assimp::ExportProperties props;

    const aiReturn status = exporter.Export(guard.get(), "obj",
                                            reinterpret_cast<const char*>(file_path.u8string().data()), 0, &props);
    if (status != AI_SUCCESS) {
        throw std::runtime_error("MeshExporter::exportAsObj: failed to export mesh to obj, " + std::string(exporter.GetErrorString()));
    }
}

V_MESHIO_NS_END
