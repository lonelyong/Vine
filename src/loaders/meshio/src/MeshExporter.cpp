#include <vine/meshio/MeshExporter.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <assimp/Exporter.hpp>
#include <assimp/material.h>
#include <assimp/scene.h>

#include <vine/Colorf.hpp>
#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/geometry/ColorMaterial.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/Material.hpp>
#include <vine/geometry/Model.hpp>
#include <vine/geometry/PbrMaterial.hpp>
#include <vine/geometry/PhongMaterial.hpp>
#include <vine/geometry/TriangleMesh.hpp>
#include <vine/io/IoError.hpp>

VN_MESHIO_NS_BEGIN

namespace
{

using IndexedTriangleMesh = vn::geometry::IndexedTriangleMesh;
using Material            = vn::geometry::Material;
using Mesh                = vn::geometry::Mesh;
using Model               = vn::geometry::Model;
using TriangleMesh        = vn::geometry::TriangleMesh;
using Vec3f               = vn::math::Vec3f;

/**
 * @brief Copies one per-vertex channel into the bare array an aiMesh holds, converting each element on the way.
 *
 * An aiMesh owns raw arrays, so a channel is always a `new[]` and a loop; the conversion is the only part that differs
 * between them, which is why a channel states only that.
 *
 * @param channel The source values, one per vertex.
 * @param convert Turns one source value into the assimp one.
 * @return The new array; the scene that holds the mesh releases it.
 */
template <typename Target, typename Source, typename Convert>
Target* copyChannel(std::span<const Source> channel, Convert convert)
{
    auto* array = new Target[channel.size()];
    for (std::size_t i = 0; i < channel.size(); ++i) {
        array[i] = convert(channel[i]);
    }
    return array;
}

/**
 * @brief View over the geometry of a triangle mesh, independent of whether it is stored as indexed or non-indexed.
 *
 * An empty `indices` view means "non-indexed": a scene is only built for a mesh that passed `isValid()`, and a valid
 * indexed mesh always holds at least one triangle.
 */
struct MeshView {
    /// Vertex positions; never empty for a valid triangle mesh.
    std::span<const Vec3f> positions;
    /// Optional per-vertex normals (empty when absent).
    std::span<const Vec3f> normals;
    /// Optional per-vertex texture coordinates (empty when absent).
    std::span<const vn::math::Vec2f> texcoords;
    /// Optional per-vertex colours, linear RGBA (empty when absent).
    std::span<const vn::math::Vec4f> colors;
    /// Triangle indices; empty for a non-indexed mesh.
    std::span<const std::uint32_t> indices;
};

/**
 * @brief What both scene writers share: the options a mesh's vertices are scaled by, and the ONE translation from a
 * Vine mesh to an aiMesh.
 *
 * Private inheritance in both writers, deliberately: the base is a tool, not an interface, so nothing outside the two
 * writers can use it. The split is the one the two entry shapes force - the mesh translation is needed by both, the
 * materials and the node tree only by ModelSceneWriter - while naming a format and writing a file belong to neither and
 * stay free functions below.
 */
class SceneWriterBase {
  protected:
    /**
     * @brief Binds the writer to the options its scale factor comes from.
     *
     * @param options The export options; they outlive the writer.
     */
    explicit SceneWriterBase(const MeshExporter::Options& options) noexcept
      : options_(options)
    {}

    /**
     * @brief Fills one aiMesh from a Vine mesh.
     *
     * The ONE place a Vine mesh becomes an assimp mesh: the name, the scaled positions, the normals, the texture
     * coordinates, the vertex colours and the triangle corners. An attribute whose length does not match the vertex
     * count is dropped, because a reader would go past its array. A format that has nowhere to put an attribute (binary
     * STL and texture coordinates, say) drops it itself - the scene states what the mesh holds, and the writer decides
     * what it can spell.
     *
     * @param io_mesh The assimp mesh to fill; the scene that holds it releases what is allocated here.
     * @param mesh The source mesh.
     * @throws std::invalid_argument when the mesh is invalid.
     * @throws std::runtime_error when the mesh type is unsupported.
     */
    void fillMesh(aiMesh& io_mesh, const Mesh& mesh) const;

  private:
    /**
     * @brief Reads the geometry of a mesh through either triangle representation.
     *
     * @param mesh The source mesh.
     * @return The geometry views.
     * @throws std::invalid_argument when the mesh is invalid.
     * @throws std::runtime_error when the mesh type is unsupported.
     */
    static MeshView viewOf(const Mesh& mesh);

    const MeshExporter::Options& options_; // The export options the scale factor comes from.
};

MeshView SceneWriterBase::viewOf(const Mesh& mesh)
{
    if (!mesh.isValid()) {
        throw std::invalid_argument("MeshExporter: mesh is invalid");
    }

    switch (mesh.shapeType()) {
    case vn::geometry::ShapeType::IndexedTriangleMesh:
    {
        const auto& itm = obj_cast<IndexedTriangleMesh>(mesh);
        return { itm.positions(), itm.normals(), itm.texcoords(), itm.colors(), itm.indices() };
    }
    case vn::geometry::ShapeType::TriangleMesh:
    {
        const auto& tm = obj_cast<TriangleMesh>(mesh);
        return { tm.positions(), tm.normals(), tm.texcoords(), tm.colors(), {} };
    }
    default: throw std::runtime_error("MeshExporter: unsupported mesh type");
    }
}

void SceneWriterBase::fillMesh(aiMesh& io_mesh, const Mesh& mesh) const
{
    const MeshView view = viewOf(mesh);

    const vn::String& name = mesh.name();
    if (!name.empty()) {
        io_mesh.mName.Set(reinterpret_cast<const char*>(name.c_str()));
    }
    io_mesh.mPrimitiveTypes = aiPrimitiveType_TRIANGLE;

    if (!view.positions.empty()) {
        io_mesh.mNumVertices = static_cast<unsigned int>(view.positions.size());

        const auto factor = static_cast<ai_real>(options_.scale_factor);
        io_mesh.mVertices = copyChannel<aiVector3D>(view.positions, [factor](const Vec3f& v) { return aiVector3D(v.x * factor, v.y * factor, v.z * factor); });

        // An attribute is written only when its array covers every vertex, so a partial one is absent rather than read
        // past; an attribute the mesh does not hold is simply not there.
        if (view.normals.size() == view.positions.size()) {
            io_mesh.mNormals = copyChannel<aiVector3D>(view.normals, [](const Vec3f& n) { return aiVector3D(n.x, n.y, n.z); });
        }

        if (view.texcoords.size() == view.positions.size()) {
            io_mesh.mNumUVComponents[0] = 2;
            io_mesh.mTextureCoords[0]   = copyChannel<aiVector3D>(view.texcoords, [](const vn::math::Vec2f& t) { return aiVector3D(t.x, t.y, 0.0f); });
        }

        // The colour set a material multiplies into its base colour: four channels per vertex.
        if (view.colors.size() == view.positions.size()) {
            io_mesh.mColors[0] = copyChannel<aiColor4D>(view.colors, [](const vn::math::Vec4f& c) { return aiColor4D(c.x, c.y, c.z, c.w); });
        }
    }

    const std::size_t triangle_count = view.indices.empty() ? view.positions.size() / 3 : view.indices.size() / 3;
    io_mesh.mNumFaces                = static_cast<unsigned int>(triangle_count);
    io_mesh.mFaces                   = new aiFace[io_mesh.mNumFaces];
    for (unsigned int i = 0; i < io_mesh.mNumFaces; ++i) {
        const unsigned int base       = i * 3;
        io_mesh.mFaces[i].mNumIndices = 3;
        // A non-indexed mesh names its corners by position, so triangle i is the vertices 3i, 3i+1 and 3i+2.
        io_mesh.mFaces[i].mIndices    = view.indices.empty() ? new unsigned int[3]{ base, base + 1, base + 2 }
                                                             : new unsigned int[3]{ view.indices[base], view.indices[base + 1], view.indices[base + 2] };
    }
}

/**
 * @brief Writes one mesh: a scene with that mesh, the default material, and a root node that names it.
 *
 * This is the whole of the mesh entry - the materials and the structure a model carries are what a caller of it does
 * not have.
 */
class MeshSceneWriter final : private SceneWriterBase {
  public:
    /**
     * @brief Builds the scene for @p mesh.
     *
     * @param options The export options (scale factor).
     * @param mesh The source mesh.
     * @throws std::invalid_argument when the mesh is invalid.
     * @throws std::runtime_error when the mesh type is unsupported.
     */
    MeshSceneWriter(const MeshExporter::Options& options, const Mesh& mesh);

    /**
     * @brief Returns the scene that was built.
     *
     * @return The scene; it stays valid as long as this writer lives.
     */
    [[nodiscard]] const aiScene& scene() const noexcept
    {
        return *scene_;
    }

  private:
    /// The scene being built; aiScene releases the root node, meshes, faces and materials it holds.
    std::unique_ptr<aiScene> scene_{ std::make_unique<aiScene>() };
};

MeshSceneWriter::MeshSceneWriter(const MeshExporter::Options& options, const Mesh& mesh)
  : SceneWriterBase(options)
{
    aiScene& ai_scene = *scene_;

    ai_scene.mMaterials    = new aiMaterial*[1]{ new aiMaterial() }; // the default a mesh without a material uses
    ai_scene.mNumMaterials = 1;

    ai_scene.mMeshes    = new aiMesh*[1]{ new aiMesh{} };
    ai_scene.mNumMeshes = 1;

    ai_scene.mRootNode             = new aiNode{};
    ai_scene.mRootNode->mMeshes    = new unsigned int[1]{ 0 };
    ai_scene.mRootNode->mNumMeshes = 1;

    fillMesh(*ai_scene.mMeshes[0], mesh);
}

/**
 * @brief Writes a model: its node tree, one mesh per entry in its own space, and the materials those entries name.
 *
 * Nothing is baked - a node's transform stays on the node, and a mesh two nodes refer to is written once - which is the
 * whole difference from MeshSceneWriter. Material index 0 is the default a mesh without a material uses; every distinct
 * material the entries refer to takes the next index, so a material two entries share is written once.
 */
class ModelSceneWriter final : private SceneWriterBase {
  private:
    /// The index each distinct material was written at, keyed by the material it came from.
    using MaterialIndexMap = std::unordered_map<const Material*, unsigned int>;

  public:
    /**
     * @brief Builds the scene for @p model.
     *
     * @param options The export options (scale factor).
     * @param model The source model; it outlives the writer.
     * @throws std::invalid_argument when the model is malformed.
     * @throws std::runtime_error when a mesh type is unsupported.
     */
    ModelSceneWriter(const MeshExporter::Options& options, const Model& model);

    /**
     * @brief Returns the scene that was built.
     *
     * @return The scene; it stays valid as long as this writer lives.
     */
    [[nodiscard]] const aiScene& scene() const noexcept
    {
        return *scene_;
    }

  private:
    /**
     * @brief Writes the default material and one per distinct material the entries refer to.
     *
     * @return The index of every material written, keyed by the material it came from.
     */
    MaterialIndexMap writeMaterials();

    /**
     * @brief Writes one mesh per entry, each pointed at the material its entry names.
     *
     * @param material_indices The indices writeMaterials() assigned.
     */
    void writeMeshes(const MaterialIndexMap& material_indices);

    /**
     * @brief Builds one aiNode subtree from a model node.
     *
     * @param index The node index to build; the model being written holds it.
     * @return The new node; the scene that owns the subtree releases it.
     */
    aiNode* buildNode(std::uint32_t index) const;

    /**
     * @brief Checks the indices the model's nodes and entries carry.
     *
     * A model is a set of arrays that refer to each other, so an index that points outside its array is a broken model,
     * not something to read past.
     *
     * @throws std::invalid_argument when the root, a child index, a mesh index or an entry is missing.
     */
    void validate() const;

    /**
     * @brief Writes one colour property in the layout an assimp material key names.
     *
     * @param io_material The material to add to.
     * @param color The colour.
     * @param key The property key, e.g. AI_MATKEY_COLOR_DIFFUSE.
     * @param type The key's type part.
     * @param index The key's index part.
     */
    static void addColor(aiMaterial& io_material, const vn::Colorf& color, const char* key, unsigned int type, unsigned int index);

    /**
     * @brief Fills one aiMaterial from a Vine material.
     *
     * The keys mirror the ones the loader reads, so a model written and read back keeps its material category and
     * colours; a PBR material is written through the metallic/roughness factors, a Phong material through its specular
     * terms.
     *
     * @param io_material The assimp material to fill.
     * @param material The source material.
     */
    static void fillMaterial(aiMaterial& io_material, const Material& material);

    /**
     * @brief Converts a Vine colour into an assimp one.
     *
     * @param color The source colour (linear RGBA).
     * @return The same colour in assimp's representation.
     */
    static aiColor4D toAiColor(const vn::Colorf& color);

    /**
     * @brief Converts a Vine model matrix into the assimp layout.
     *
     * Both use the column-vector convention with the translation in the fourth column, so the elements map one to one.
     *
     * @param transform The source matrix.
     * @return The same matrix in assimp's representation.
     */
    static aiMatrix4x4 toAiMatrix(const vn::math::Mat4d& transform);

    const Model& model_; // The model being written; the node indices refer to it.
    /// The scene being built; aiScene releases the root node, meshes, faces and materials it holds.
    std::unique_ptr<aiScene> scene_{ std::make_unique<aiScene>() };
};

ModelSceneWriter::ModelSceneWriter(const MeshExporter::Options& options, const Model& model)
  : SceneWriterBase(options)
  , model_(model)
{
    validate();

    // A mesh names its material by index, so the material array is numbered first and the meshes then look theirs up.
    const MaterialIndexMap material_indices = writeMaterials();
    writeMeshes(material_indices);

    scene_->mRootNode = buildNode(model_.root());
}

void ModelSceneWriter::addColor(aiMaterial& io_material, const vn::Colorf& color, const char* key, unsigned int type, unsigned int index)
{
    const aiColor4D value = toAiColor(color);
    io_material.AddProperty(&value, 1, key, type, index);
}

aiColor4D ModelSceneWriter::toAiColor(const vn::Colorf& color)
{
    return aiColor4D(static_cast<ai_real>(color.r), static_cast<ai_real>(color.g), static_cast<ai_real>(color.b), static_cast<ai_real>(color.a));
}

void ModelSceneWriter::fillMaterial(aiMaterial& io_material, const Material& material)
{
    if (!material.name().empty()) {
        aiString name;
        name.Set(reinterpret_cast<const char*>(material.name().c_str()));
        io_material.AddProperty(&name, AI_MATKEY_NAME);
    }

    switch (material.materialType()) {
    case vn::geometry::MaterialType::Color:
    {
        const auto& color = static_cast<const vn::geometry::ColorMaterial&>(material).color();
        addColor(io_material, color, AI_MATKEY_COLOR_DIFFUSE);
        addColor(io_material, color, AI_MATKEY_BASE_COLOR);
        const float opacity = color.a;
        io_material.AddProperty(&opacity, 1, AI_MATKEY_OPACITY);
        break;
    }
    case vn::geometry::MaterialType::Phong:
    {
        const auto& phong = static_cast<const vn::geometry::PhongMaterial&>(material);
        addColor(io_material, phong.ambient(), AI_MATKEY_COLOR_AMBIENT);
        addColor(io_material, phong.diffuse(), AI_MATKEY_COLOR_DIFFUSE);
        addColor(io_material, phong.diffuse(), AI_MATKEY_BASE_COLOR);
        addColor(io_material, phong.specular(), AI_MATKEY_COLOR_SPECULAR);
        const float shininess = phong.shininess();
        io_material.AddProperty(&shininess, 1, AI_MATKEY_SHININESS);
        const float opacity = phong.diffuse().a;
        io_material.AddProperty(&opacity, 1, AI_MATKEY_OPACITY);
        break;
    }
    case vn::geometry::MaterialType::Pbr:
    {
        const auto& pbr = static_cast<const vn::geometry::PbrMaterial&>(material);
        addColor(io_material, pbr.baseColor(), AI_MATKEY_COLOR_DIFFUSE);
        addColor(io_material, pbr.baseColor(), AI_MATKEY_BASE_COLOR);
        addColor(io_material, pbr.emissive(), AI_MATKEY_COLOR_EMISSIVE);
        const float metallic = pbr.metallic();
        io_material.AddProperty(&metallic, 1, AI_MATKEY_METALLIC_FACTOR);
        const float roughness = pbr.roughness();
        io_material.AddProperty(&roughness, 1, AI_MATKEY_ROUGHNESS_FACTOR);
        const float opacity = pbr.opacity();
        io_material.AddProperty(&opacity, 1, AI_MATKEY_OPACITY);
        break;
    }
    default: break;
    }
}

aiMatrix4x4 ModelSceneWriter::toAiMatrix(const vn::math::Mat4d& transform)
{
    aiMatrix4x4 result;
    result.a1 = static_cast<ai_real>(transform.element(0, 0));
    result.a2 = static_cast<ai_real>(transform.element(0, 1));
    result.a3 = static_cast<ai_real>(transform.element(0, 2));
    result.a4 = static_cast<ai_real>(transform.element(0, 3));
    result.b1 = static_cast<ai_real>(transform.element(1, 0));
    result.b2 = static_cast<ai_real>(transform.element(1, 1));
    result.b3 = static_cast<ai_real>(transform.element(1, 2));
    result.b4 = static_cast<ai_real>(transform.element(1, 3));
    result.c1 = static_cast<ai_real>(transform.element(2, 0));
    result.c2 = static_cast<ai_real>(transform.element(2, 1));
    result.c3 = static_cast<ai_real>(transform.element(2, 2));
    result.c4 = static_cast<ai_real>(transform.element(2, 3));
    result.d1 = static_cast<ai_real>(transform.element(3, 0));
    result.d2 = static_cast<ai_real>(transform.element(3, 1));
    result.d3 = static_cast<ai_real>(transform.element(3, 2));
    result.d4 = static_cast<ai_real>(transform.element(3, 3));
    return result;
}

aiNode* ModelSceneWriter::buildNode(std::uint32_t index) const
{
    const Model::Node& source = model_.nodes()[index];

    auto node = std::make_unique<aiNode>();
    if (!source.name.empty()) {
        node->mName.Set(reinterpret_cast<const char*>(source.name.c_str()));
    }
    node->mTransformation = toAiMatrix(source.transform);

    node->mNumMeshes = static_cast<unsigned int>(source.meshes.size());
    if (node->mNumMeshes > 0) {
        node->mMeshes = new unsigned int[node->mNumMeshes];
        for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
            node->mMeshes[i] = source.meshes[i];
        }
    }

    node->mNumChildren = static_cast<unsigned int>(source.children.size());
    if (node->mNumChildren > 0) {
        // Zero-initialised so a failure while building a later child leaves no undetermined pointer to delete.
        node->mChildren = new aiNode*[node->mNumChildren]();
        for (unsigned int i = 0; i < node->mNumChildren; ++i) {
            node->mChildren[i] = buildNode(source.children[i]);
        }
    }
    return node.release();
}

void ModelSceneWriter::validate() const
{
    const std::vector<Model::Node>& nodes = model_.nodes();
    if (nodes.empty() || model_.root() >= nodes.size()) {
        throw std::invalid_argument("MeshExporter: the model has no root node");
    }

    for (const Model::Node& node : nodes) {
        for (const std::uint32_t child : node.children) {
            if (child >= nodes.size()) {
                throw std::invalid_argument("MeshExporter: a node child index is out of range");
            }
        }
        for (const std::uint32_t mesh : node.meshes) {
            if (mesh >= model_.meshes().size()) {
                throw std::invalid_argument("MeshExporter: a node mesh index is out of range");
            }
        }
    }

    for (const Model::Entry& entry : model_.meshes()) {
        if (!entry.mesh) {
            throw std::invalid_argument("MeshExporter: the model holds an empty mesh entry");
        }
    }
}

ModelSceneWriter::MaterialIndexMap ModelSceneWriter::writeMaterials()
{
    const std::vector<Model::Entry>& entries = model_.meshes();

    // Two entries may share a material, so this pass both numbers the distinct ones and records where each went.
    std::vector<const Material*> order;
    order.reserve(entries.size());
    MaterialIndexMap indices;
    for (const Model::Entry& entry : entries) {
        const Material* const material = entry.material.get();
        if (material == nullptr || indices.contains(material)) {
            continue; // no material, or one an earlier entry already numbered
        }
        indices.emplace(material, static_cast<unsigned int>(order.size()) + 1);
        order.push_back(material);
    }

    aiScene& ai_scene      = *scene_;
    ai_scene.mMaterials    = new aiMaterial*[order.size() + 1]();
    ai_scene.mNumMaterials = static_cast<unsigned int>(order.size() + 1);
    ai_scene.mMaterials[0] = new aiMaterial(); // index 0 is the default a mesh without a material uses
    for (std::size_t i = 0; i < order.size(); ++i) {
        ai_scene.mMaterials[i + 1] = new aiMaterial();
        fillMaterial(*ai_scene.mMaterials[i + 1], *order[i]);
    }
    return indices;
}

void ModelSceneWriter::writeMeshes(const MaterialIndexMap& material_indices)
{
    const std::vector<Model::Entry>& entries = model_.meshes();
    if (entries.empty()) {
        return; // a scene without meshes keeps the null array, which every writer accepts
    }

    aiScene& ai_scene   = *scene_;
    ai_scene.mMeshes    = new aiMesh*[entries.size()]();
    ai_scene.mNumMeshes = static_cast<unsigned int>(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Model::Entry& entry = entries[i];
        ai_scene.mMeshes[i]       = new aiMesh{};
        fillMesh(*ai_scene.mMeshes[i], *entry.mesh);
        if (entry.material) {
            ai_scene.mMeshes[i]->mMaterialIndex = material_indices.at(entry.material.get());
        }
    }
}

/**
 * @brief Maps a format to the writer id this assimp build registers.
 *
 * @param format The target format.
 * @param encoding The binary/text choice; only a format whose info reports an encoding choice responds to it.
 * @param keeps_side_files Whether the target can hold a companion file (a path can, a stream cannot).
 * @return The writer id, or null for MeshFormat::Unknown.
 */
const char* writerId(MeshFormat format, MeshExporter::Encoding encoding, bool keeps_side_files) noexcept
{
    switch (format) {
    case MeshFormat::Stl: return encoding == MeshExporter::Encoding::Text ? "stl" : "stlb";
    case MeshFormat::Obj: return keeps_side_files ? "obj" : "objnomtl";
    case MeshFormat::Ply: return encoding == MeshExporter::Encoding::Text ? "ply" : "plyb";
    case MeshFormat::Gltf: return "gltf2";
    case MeshFormat::Glb: return "glb2";
    case MeshFormat::Fbx: return encoding == MeshExporter::Encoding::Text ? "fbxa" : "fbx";
    case MeshFormat::Collada: return "collada";
    case MeshFormat::ThreeMf: return "3mf";
    case MeshFormat::Unknown: break;
    }
    return nullptr;
}

/**
 * @brief Reports whether this build's writer knows a format id.
 *
 * @param exporter The writer whose registry is checked.
 * @param format_id The writer id to look for.
 * @return true when the id is registered.
 */
bool writerExists(const Assimp::Exporter& exporter, const char* format_id)
{
    for (std::size_t i = 0; i < exporter.GetExportFormatCount(); ++i) {
        const aiExportFormatDesc* desc = exporter.GetExportFormatDescription(i);
        if (desc != nullptr && desc->id != nullptr && std::strcmp(format_id, desc->id) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Writes a scene to a file, creating missing parent directories.
 *
 * @param scene The scene to write.
 * @param format_id The writer id.
 * @param file_path The output file path.
 * @return IoError::Ok on success; Unsupported when this build has no such writer, IoFailure when the file cannot be
 *         written.
 */
vn::io::IoError writeScene(const aiScene& scene, const char* format_id, const std::filesystem::path& file_path)
{
    Assimp::Exporter exporter;
    if (!writerExists(exporter, format_id)) {
        return vn::io::IoError::Unsupported;
    }

    try {
        if (file_path.has_parent_path()) {
            std::filesystem::create_directories(file_path.parent_path());
        }
    }
    catch (const std::exception&) {
        return vn::io::IoError::IoFailure;
    }

    Assimp::ExportProperties properties;
    const aiReturn           status = exporter.Export(&scene, format_id, reinterpret_cast<const char*>(file_path.u8string().data()), 0, &properties);
    return status == AI_SUCCESS ? vn::io::IoError::Ok : vn::io::IoError::IoFailure;
}

/**
 * @brief Writes a scene into a stream through the writer's memory image.
 *
 * The writer has no stream target: the whole file is built first and handed over afterwards. That is the price of not
 * naming a file.
 *
 * @param scene The scene to write.
 * @param format_id The writer id.
 * @param out The stream to write; it must outlive the call.
 * @return IoError::Ok on success; Unsupported when this build has no such writer or it produces more than one file,
 *         IoFailure when the writer or the stream fails.
 */
vn::io::IoError writeScene(const aiScene& scene, const char* format_id, std::ostream& out)
{
    Assimp::Exporter exporter;
    if (!writerExists(exporter, format_id)) {
        return vn::io::IoError::Unsupported;
    }

    Assimp::ExportProperties properties;
    const aiExportDataBlob*  blob = exporter.ExportToBlob(&scene, format_id, 0, &properties);
    if (blob == nullptr || blob->data == nullptr) {
        return vn::io::IoError::IoFailure;
    }
    if (blob->next != nullptr) {
        return vn::io::IoError::Unsupported; // one stream holds one file, and this writer produced several
    }

    out.write(static_cast<const char*>(blob->data), static_cast<std::streamsize>(blob->size));
    return out ? vn::io::IoError::Ok : vn::io::IoError::IoFailure;
}

/**
 * @brief A data source whose bytes are produced by an exporter on the first pull.
 *
 * Nothing is written at construction. The first size() or read() runs the producer into a memory image, after which the
 * bytes are held here and rewind() replays them - a package that saves twice exports once. A producer failure is
 * reported through error(), the way every other source reports one.
 */
class ExportedBlobSource final : public vn::io::DataSource {
  public:
    /// Writes the whole content into the stream it is handed, and reports why it could not.
    using Producer = std::function<vn::io::IoError(std::ostream&)>;

    /**
     * @brief Creates a source that runs the producer on the first pull.
     *
     * @param producer The writer; it is called at most once.
     */
    explicit ExportedBlobSource(Producer producer)
      : producer_(std::move(producer))
    {}

    [[nodiscard]] std::uint64_t size() const override
    {
        produce();
        return static_cast<std::uint64_t>(bytes_.size());
    }

    void rewind() override
    {
        cursor_ = 0;
    }

    [[nodiscard]] std::size_t read(std::span<std::byte> out) override
    {
        produce();
        if (cursor_ >= bytes_.size()) {
            return 0;
        }
        const std::size_t count = std::min(out.size(), bytes_.size() - cursor_);
        std::memcpy(out.data(), bytes_.data() + cursor_, count);
        cursor_ += count;
        return count;
    }

    [[nodiscard]] vn::io::IoError error() const override
    {
        return error_;
    }

  private:
    /// Runs the producer once; its failure is remembered and reported through error().
    void produce() const
    {
        if (produced_) {
            return;
        }
        produced_ = true;

        std::ostringstream image(std::ios::binary);
        try {
            error_ = producer_(image);
        }
        catch (const std::exception&) {
            error_ = vn::io::IoError::IoFailure;
            return;
        }
        if (error_ == vn::io::IoError::Ok) {
            bytes_ = image.str();
        }
    }

    Producer                producer_;
    mutable std::string     bytes_;
    mutable bool            produced_{ false };
    mutable vn::io::IoError error_{ vn::io::IoError::Ok };
    std::size_t             cursor_{ 0 };
};

/**
 * @brief A source that produces nothing and states why.
 *
 * createSource() answers a source for every format, so a format the target cannot serve (no single self-contained file)
 * is reported through error() instead of by returning null: a container asks that question before it takes the source.
 */
class RefusedSource final : public vn::io::DataSource {
  public:
    /**
     * @brief Creates a source that reports @p error and yields no bytes.
     *
     * @param error The failure to report.
     */
    explicit RefusedSource(vn::io::IoError error) noexcept
      : error_(error)
    {}

    [[nodiscard]] std::uint64_t size() const override
    {
        return 0;
    }

    void rewind() override
    {}

    [[nodiscard]] std::size_t read(std::span<std::byte>) override
    {
        return 0;
    }

    [[nodiscard]] vn::io::IoError error() const override
    {
        return error_;
    }

  private:
    vn::io::IoError error_;
};

} // namespace

MeshExporter::MeshExporter() = default;

MeshExporter::MeshExporter(Options options)
  : options_(std::move(options))
{}

MeshExporter::~MeshExporter() = default;

const MeshExporter::Options& MeshExporter::options() const noexcept
{
    return options_;
}

vn::io::IoError MeshExporter::exportAs(const Mesh& mesh, MeshFormat format, const std::filesystem::path& file_path) const
{
    const char* const format_id = writerId(format, options_.encoding, /*keeps_side_files=*/true);
    if (format_id == nullptr) {
        return vn::io::IoError::Unsupported;
    }

    try {
        const MeshSceneWriter writer(options_, mesh);
        return writeScene(writer.scene(), format_id, file_path);
    }
    catch (const std::exception&) {
        return vn::io::IoError::InvalidData; // the mesh is not geometry this writer can state
    }
}

vn::io::IoError MeshExporter::exportAs(const Model& model, MeshFormat format, const std::filesystem::path& file_path) const
{
    const char* const format_id = writerId(format, options_.encoding, /*keeps_side_files=*/true);
    if (format_id == nullptr) {
        return vn::io::IoError::Unsupported;
    }

    try {
        const ModelSceneWriter writer(options_, model);
        return writeScene(writer.scene(), format_id, file_path);
    }
    catch (const std::exception&) {
        return vn::io::IoError::InvalidData; // the model is malformed, or holds a mesh this writer cannot state
    }
}

vn::io::IoError MeshExporter::exportAs(const Mesh& mesh, MeshFormat format, std::ostream& out) const
{
    if (!formatInfo(format).streamable) {
        return vn::io::IoError::Unsupported; // one stream holds one file, and this format's normal form is two
    }
    const char* const format_id = writerId(format, options_.encoding, /*keeps_side_files=*/false);
    if (format_id == nullptr) {
        return vn::io::IoError::Unsupported;
    }

    try {
        const MeshSceneWriter writer(options_, mesh);
        return writeScene(writer.scene(), format_id, out);
    }
    catch (const std::exception&) {
        return vn::io::IoError::InvalidData;
    }
}

vn::io::IoError MeshExporter::exportAs(const Model& model, MeshFormat format, std::ostream& out) const
{
    if (!formatInfo(format).streamable) {
        return vn::io::IoError::Unsupported;
    }

    // A stream cannot hold the companion a path target gets (OBJ's .mtl, glTF's .bin): the geometry, the names and the
    // node tree are kept, the companion is not.
    const char* const format_id = writerId(format, options_.encoding, /*keeps_side_files=*/false);
    if (format_id == nullptr) {
        return vn::io::IoError::Unsupported;
    }

    try {
        const ModelSceneWriter writer(options_, model);
        return writeScene(writer.scene(), format_id, out);
    }
    catch (const std::exception&) {
        return vn::io::IoError::InvalidData;
    }
}

std::shared_ptr<vn::io::DataSource> MeshExporter::createSource(vn::intrusive_ptr<const Mesh> mesh, MeshFormat format) const
{
    if (!formatInfo(format).streamable) {
        return std::make_shared<RefusedSource>(vn::io::IoError::Unsupported);
    }

    const Options options = options_;
    return std::make_shared<ExportedBlobSource>([mesh = std::move(mesh), format, options](std::ostream& out) {
        if (!mesh) {
            return vn::io::IoError::InvalidData; // a null mesh is not content to write
        }
        return MeshExporter(options).exportAs(*mesh, format, out);
    });
}

std::shared_ptr<vn::io::DataSource> MeshExporter::createSource(vn::intrusive_ptr<const Model> model, MeshFormat format) const
{
    if (!formatInfo(format).streamable) {
        return std::make_shared<RefusedSource>(vn::io::IoError::Unsupported);
    }

    const Options options = options_;
    return std::make_shared<ExportedBlobSource>([model = std::move(model), format, options](std::ostream& out) {
        if (!model) {
            return vn::io::IoError::InvalidData;
        }
        return MeshExporter(options).exportAs(*model, format, out);
    });
}

VN_MESHIO_NS_END
