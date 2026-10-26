#pragma once

#include "meshio_global.hpp"

#include <filesystem>
#include <memory>
#include <ostream>

#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/geometry/Mesh.hpp>
#include <vine/geometry/Model.hpp>
#include <vine/io/Stream.hpp>

VN_MESHIO_NS_BEGIN

/**
 * @brief Utility class for exporting meshes and models (STL, OBJ, ...).
 *
 * exportAs() names the format at the call, and the same set of entries serves a mesh and a model: a mesh is one
 * geometry, a model keeps the node tree, the separate meshes and the materials a source described.
 *
 * WHAT A FORMAT CANNOT SPELL IS DROPPED, NOT REPORTED: the scene states every attribute the mesh holds - normals,
 * texture coordinates, vertex colours - and the writer keeps the ones its format has a place for. Binary STL, for one,
 * holds nothing per vertex, so a coloured mesh exported as STL arrives without the colours. Ask for a format that
 * carries what the mesh holds (PLY, glTF2, FBX) when losing an attribute matters.
 */
class VN_MESHIO_API MeshExporter
{
    // 类型声明区块
  public:
    /** @brief Export options. */
    struct Options
    {
        /** @brief Vertex scale factor applied on export; 1.0 means no scaling. */
        double scale_factor{ 1.0 };

        /** @brief Whether the target format is written as binary or text. */
        enum class Format
        {
            /// Binary format (STL, glTF, PLY, FBX, ...).
            Binary,
            /// Text format (STL, glTF, PLY, FBX, OBJ, ...).
            Ascii,
        };

        /// Binary or text; ignored when the target format does not support it.
        Format format{ Format::Binary };
    };

    // 构造函数区块
  public:
    /**
     * @brief Creates an exporter with @p options.
     *
     * @details
     * - The options belong to THIS exporter, so two callers that need different scale factors or encodings each keep
     *   their own instead of taking turns on one. defaultInstance() is the convenience instance for a caller that
     *   wants the defaults and does not mind sharing.
     *
     * @param options The export options; the default states scale 1.0 and binary output.
     */
    explicit MeshExporter(Options options = {});
    MeshExporter(const MeshExporter&) = delete;
    MeshExporter(MeshExporter&&) = delete;
    ~MeshExporter();

    // 方法区块
  public:
    /**
     * @brief Returns the shared default exporter instance.
     *
     * @return The singleton instance.
     */
    static MeshExporter& defaultInstance();

    /**
     * @brief Returns the export options.
     *
     * @return The options this exporter was constructed with.
     */
    [[nodiscard]] const Options& options() const noexcept;

    /**
     * @brief Exports a mesh as the named format, side files included.
     *
     * @details
     * - format is either a family whose binary/text variant Options::format picks ("stl", "ply", "fbx"), or an
     *   explicit writer format name ("obj", "objnomtl", "collada", "gltf2", "3mf", ...); it is the format name, not
     *   the file extension, that selects the writer.
     * - A format that writes side files (the OBJ material library) places them next to file_path.
     * - Options::scale_factor scales the vertices; nothing else about the mesh is changed.
     *
     * @param mesh The mesh to export.
     * @param format The target format.
     * @param file_path The output file path; missing parent directories are created.
     * @throws std::invalid_argument when the mesh is invalid.
     * @throws std::runtime_error when the mesh type is unsupported, the format is not available, or the export fails.
     */
    void exportAs(const vn::geometry::Mesh& mesh, const vn::String& format, const std::filesystem::path& file_path) const;

    /**
     * @brief Exports a mesh as the named format into one stream.
     *
     * @details
     * - The writer produces a memory image, so the bytes exist in full before they are handed over: this entry saves
     *   the temporary file, not the memory.
     * - One stream holds one file, so a format that writes side files is refused here; ask for "objnomtl" when the
     *   OBJ has to be self-contained.
     *
     * @param mesh The mesh to export.
     * @param format The target format.
     * @param out The stream to write; it must outlive the call.
     * @throws std::invalid_argument when the mesh is invalid.
     * @throws std::runtime_error when the mesh type is unsupported, the format is not available, the format needs more
     *         than one file, or the stream refuses the bytes.
     */
    void exportAs(const vn::geometry::Mesh& mesh, const vn::String& format, std::ostream& out) const;

    /**
     * @brief Exports a whole model as the named format, keeping its structure.
     *
     * @details
     * - The node tree, every mesh and the material each mesh uses are written as the source described them; a node's
     *   transform stays on the node instead of being baked into its vertices.
     * - Options::scale_factor scales the vertices of every mesh; node transforms are written unchanged, so a scaled
     *   model keeps the layout its nodes state.
     * - A format that writes side files (the OBJ material library) places them next to file_path.
     *
     * @param model The model to export.
     * @param format The target format.
     * @param file_path The output file path; missing parent directories are created.
     * @throws std::invalid_argument when the model has no root node or holds an empty mesh entry.
     * @throws std::runtime_error when a mesh type is unsupported, the format is not available, or the export fails.
     */
    void exportAs(const vn::geometry::Model& model, const vn::String& format, const std::filesystem::path& file_path) const;

    /**
     * @brief Exports a whole model as the named format into one stream.
     *
     * The structure is kept exactly as the path entry keeps it; one stream holds one file, so a format that writes
     * side files is refused here.
     *
     * @param model The model to export.
     * @param format The target format.
     * @param out The stream to write; it must outlive the call.
     * @throws std::invalid_argument when the model has no root node or holds an empty mesh entry.
     * @throws std::runtime_error when a mesh type is unsupported, the format is not available, the format needs more
     *         than one file, or the stream refuses the bytes.
     */
    void exportAs(const vn::geometry::Model& model, const vn::String& format, std::ostream& out) const;

    /**
     * @brief Creates a source that exports a mesh on demand.
     *
     * @details
     * - Nothing is written here: the first pull (size() or read()) produces the content, and rewind() replays the same
     *   bytes, so a package saved twice exports once.
     * - The source holds the mesh, and the options are the ones in effect when it is created.
     * - The content is produced by the writer exportAs(stream) uses, so the whole file exists in memory once it has
     *   been pulled. BinStlSource, AsciiStlSource and ObjSource stay the record-by-record alternative for their formats.
     * - A format that writes side files cannot be produced into one source; exportAs(path) is the entry for those.
     *
     * @param mesh The mesh to export; the source holds it.
     * @param format The target format; the options' snapshot resolves a family name the same way exportAs() does.
     * @return The source; never null. A mesh the writer refuses surfaces through the source's error(), not here.
     */
    std::shared_ptr<vn::io::DataSource> createSource(const vn::intrusive_ptr<const vn::geometry::Mesh>& mesh,
                                                     vn::String                                       format) const;

    /**
     * @brief Creates a source that exports a whole model on demand.
     *
     * Like the mesh entry, but the content keeps the node tree, the separate meshes and the materials.
     *
     * @param model The model to export; the source holds it.
     * @param format The target format; the options' snapshot resolves a family name the same way exportAs() does.
     * @return The source; never null. A model the writer refuses surfaces through the source's error(), not here.
     */
    std::shared_ptr<vn::io::DataSource> createSource(const vn::intrusive_ptr<const vn::geometry::Model>& model,
                                                     vn::String                                        format) const;

    // 字段区块
  private:
    Options options_;
};

VN_MESHIO_NS_END
