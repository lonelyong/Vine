#pragma once

#include "meshio_global.hpp"

#include <filesystem>

#include <vine/geometry/Mesh.hpp>

V_MESHIO_NS_BEGIN

/**
 * @brief Utility class for exporting meshes to files (STL, OBJ, ...).
 */
class V_MESHIO_API MeshExporter
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
    void exportAsStl(const vine::geometry::Mesh& mesh, const std::filesystem::path& file_path) const;

    /**
     * @brief Exports a mesh as an OBJ file.
     *
     * @param mesh The mesh to export.
     * @param file_path The output file path.
     * @throws std::runtime_error when the export fails.
     */
    void exportAsObj(const vine::geometry::Mesh& mesh, const std::filesystem::path& file_path) const;

    // 字段区块
  private:
    Options options_;
};

V_MESHIO_NS_END
