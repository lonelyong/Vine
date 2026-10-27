#pragma once

#include "meshio_global.hpp"

#include <filesystem>
#include <memory>
#include <ostream>

#include <vine/geometry/Mesh.hpp>
#include <vine/geometry/Model.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/io/IoError.hpp>
#include <vine/io/Stream.hpp>

#include "MeshFormat.hpp"

VN_MESHIO_NS_BEGIN

/**
 * @brief Exports meshes and models (STL, OBJ, glTF, ...).
 *
 * @details
 * - The TARGET decides how many files one write may produce: a path can hold a companion file (OBJ's .mtl, glTF's
 *   .bin), a stream or a data source holds exactly one self-contained file.
 * - A Model written to a format that cannot carry node transforms (STL, PLY) is flattened by the writer: the node
 *   transforms are applied and every mesh lands in the same file. Several meshes are written side by side - nothing is
 *   merged into a single mesh, and no boolean fusion is performed.
 * - WHAT A FORMAT CANNOT SPELL IS DROPPED: an attribute the target format has no place for (STL carries neither
 *   materials nor texture coordinates) is not an error. What IS an error is a target that cannot hold the content at
 *   all, which answers IoError::Unsupported.
 * - Every entry answers a vn::io::IoError instead of throwing: the writer's own failures are IoFailure, malformed
 *   geometry is InvalidData, and a target the format cannot serve is Unsupported.
 *
 * @warning
 * - An exporter is not thread-safe: one instance must not be used from two threads at the same time.
 */
class VN_MESHIO_API MeshExporter {
  public:
    /** @brief Whether a format that has both spellings is written as binary or as text. */
    enum class Encoding
    {
        /// Binary variant (STL, PLY, FBX).
        Binary,
        /// Text variant (STL, PLY, FBX).
        Text,
    };

    /** @brief Export options. */
    struct Options {
        /// Vertex scale factor applied on export; 1.0 means no scaling.
        double scale_factor{ 1.0 };

        /// Binary or text; ignored by a format whose info does not report an encoding choice.
        Encoding encoding{ Encoding::Binary };
    };

  public:
    /**
     * @brief Creates an exporter with @p options.
     *
     * @details
     * - The options belong to THIS exporter, so two callers that need different scale factors or encodings each keep
     *   their own instead of taking turns on one. There is no shared default instance.
     *
     * @param options The export options; the default states scale 1.0 and binary output.
     */
    explicit MeshExporter(Options options = {});
    MeshExporter(const MeshExporter&) = delete;
    MeshExporter(MeshExporter&&)      = delete;
    ~MeshExporter();

  public:
    /**
     * @brief Returns the export options.
     *
     * @return The options this exporter was constructed with.
     */
    [[nodiscard]] const Options& options() const noexcept;

    /**
     * @brief Writes a mesh to a file.
     *
     * @details
     * - The format is stated, not guessed from the path: a typo in a file name cannot silently produce a format the
     *   caller did not ask for.
     * - Missing parent directories are created. A mesh carries no material, so the OBJ written here is self-contained
     *   (no .mtl) and every format's output is one file.
     *
     * @param mesh      The mesh to export.
     * @param format    The target format.
     * @param file_path The output file path.
     * @return IoError::Ok on success; InvalidData when the mesh is invalid or of an unsupported type, Unsupported when
     *         the format cannot be written by this build, IoFailure when the file cannot be written.
     */
    [[nodiscard]] vn::io::IoError exportAs(const vn::geometry::Mesh& mesh, MeshFormat format, const std::filesystem::path& file_path) const;

    /**
     * @brief Writes a whole model to a file, keeping its structure.
     *
     * @details
     * - The node tree, every mesh and the material each mesh uses are written as the source described them; a node's
     *   transform stays on the node instead of being baked into its vertices. A format that cannot carry transforms
     *   (STL, PLY) is flattened by the writer instead, and the parts land in the same file.
     * - A format that writes a companion file (OBJ's .mtl, glTF's .bin) places it next to @p file_path.
     *
     * @param model     The model to export.
     * @param format    The target format.
     * @param file_path The output file path; missing parent directories are created.
     * @return IoError::Ok on success; InvalidData when the model is malformed, Unsupported when the format cannot be
     *         written by this build, IoFailure when a file cannot be written.
     */
    [[nodiscard]] vn::io::IoError exportAs(const vn::geometry::Model& model, MeshFormat format, const std::filesystem::path& file_path) const;

    /**
     * @brief Writes a mesh into one stream.
     *
     * @details
     * - One stream holds one file, so the format must be streamable (see MeshFormatInfo::streamable): a format whose
     *   model form needs a companion file is refused here with IoError::Unsupported.
     * - The content is the same file exportAs(path) would write for a mesh.
     *
     * @param mesh   The mesh to export.
     * @param format The target format.
     * @param out    The stream to write; it must outlive the call.
     * @return IoError::Ok on success; InvalidData for invalid geometry, Unsupported when the format cannot be streamed
     *         or is not available in this build, IoFailure when the stream refuses the bytes.
     */
    [[nodiscard]] vn::io::IoError exportAs(const vn::geometry::Mesh& mesh, MeshFormat format, std::ostream& out) const;

    /**
     * @brief Writes a whole model into one stream.
     *
     * @details
     * - One stream holds one file: a model written here keeps its node tree when the format can carry one, and drops
     *   the companion file a path target would get (an OBJ keeps its geometry and names, not its .mtl).
     * - A format that cannot exist as one self-contained file answers IoError::Unsupported.
     *
     * @param model  The model to export.
     * @param format The target format.
     * @param out    The stream to write; it must outlive the call.
     * @return IoError::Ok on success; InvalidData for a malformed model, Unsupported when the format cannot be streamed
     *         or is not available in this build, IoFailure when the stream refuses the bytes.
     */
    [[nodiscard]] vn::io::IoError exportAs(const vn::geometry::Model& model, MeshFormat format, std::ostream& out) const;

    /**
     * @brief Creates a source that exports a mesh on demand.
     *
     * @details
     * - This is how a mesh reaches a package (a Vfs entry) without a temporary file: the source pulls the bytes the
     *   exporter writes, so the caller never materializes one itself.
     * - Nothing is written here: the first pull (size() or read()) produces the content, and rewind() replays the same
     *   bytes, so a container saved twice exports once.
     * - The source holds the mesh; the options are the ones in effect when it is created.
     * - The bytes are produced by exportAs(stream), so a streamable format yields the same content through either
     *   entry. A format that is not streamable yields a source whose error() is IoError::Unsupported.
     *
     * @param mesh   The mesh to export; the source holds it.
     * @param format The target format.
     * @return The source; never null. A refusal surfaces through the source's error(), not here.
     */
    std::shared_ptr<vn::io::DataSource> createSource(vn::intrusive_ptr<const vn::geometry::Mesh> mesh, MeshFormat format) const;

    /**
     * @brief Creates a source that exports a whole model on demand.
     *
     * Like the mesh entry, but the content keeps the node tree, the separate meshes and the materials (except the
     * companion file a path target would carry, which one file cannot hold).
     *
     * @param model  The model to export; the source holds it.
     * @param format The target format.
     * @return The source; never null. A refusal surfaces through the source's error(), not here.
     */
    std::shared_ptr<vn::io::DataSource> createSource(vn::intrusive_ptr<const vn::geometry::Model> model, MeshFormat format) const;

  private:
    Options options_;
};

VN_MESHIO_NS_END
