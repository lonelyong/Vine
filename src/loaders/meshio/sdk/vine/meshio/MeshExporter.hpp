#pragma once

#include "meshio_global.hpp"

#include <filesystem>
#include <ostream>

#include <vine/geometry/Mesh.hpp>

VN_MESHIO_NS_BEGIN

/**
 * @brief Utility class for exporting meshes to files (STL, OBJ, ...).
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
    MeshExporter();
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
     * @return Mutable reference to the options.
     */
    Options& options() noexcept;

    /**
     * @brief Sets the export options.
     *
     * @param options The new options.
     */
    void setOptions(const Options& options);

    /**
     * @brief Exports a mesh as an STL file.
     *
     * @param mesh The mesh to export.
     * @param file_path The output file path.
     * @throws std::runtime_error when the export fails.
     */
    void exportAsStl(const vn::geometry::Mesh& mesh, const std::filesystem::path& file_path) const;

    /**
     * @brief Exports a mesh as STL into a stream.
     *
     * For callers whose output is not a file: a memory buffer, a network sink, a test. assimp writes a memory blob
     * rather than a stream, so the bytes exist in full before they are handed over - this entry saves the temporary file,
     * not the memory. Prefer the path overload when a file is the target.
     *
     * A format that writes more than one file does not fit one stream; the OBJ entry below writes assimp's
     * material-free variant for that reason.
     *
     * @param mesh The mesh to export.
     * @param out The stream to write; it must outlive the call.
     * @throws std::runtime_error when the export fails, or when the stream refuses the bytes.
     */
    void exportAsStl(const vn::geometry::Mesh& mesh, std::ostream& out) const;

    /**
     * @brief Exports a mesh as an OBJ file.
     *
     * @param mesh The mesh to export.
     * @param file_path The output file path.
     * @throws std::runtime_error when the export fails.
     */
    void exportAsObj(const vn::geometry::Mesh& mesh, const std::filesystem::path& file_path) const;

    /**
     * @brief Exports a mesh as OBJ into a stream.
     *
     * The stream form writes assimp's material-free OBJ variant: a material library is a second file, and one stream
     * holds one file. Everything the text refers to lies inside the stream, so the result parses on its own - ask for
     * the path overload when the material library is wanted. Like the STL entry, the bytes exist in full before they
     * are handed over.
     *
     * @param mesh The mesh to export.
     * @param out The stream to write; it must outlive the call.
     * @throws std::runtime_error when the export fails, or when the stream refuses the bytes.
     */
    void exportAsObj(const vn::geometry::Mesh& mesh, std::ostream& out) const;

    // 字段区块
  private:
    Options options_;
};

VN_MESHIO_NS_END
