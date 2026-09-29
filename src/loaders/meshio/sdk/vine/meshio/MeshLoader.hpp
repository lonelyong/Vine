#pragma once

#include "meshio_global.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <istream>
#include <unordered_map>
#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/crypto/ByteSequenceFingerprint.hpp>
#include <vine/geometry/Mesh.hpp>
#include <vine/geometry/Model.hpp>
#include <vine/runtime/InMemoryCache.hpp>

VN_MESHIO_NS_BEGIN

/**
 * @brief Utility class for loading mesh models (STL, OBJ, ...).
 *
 * Loaded meshes are cached by content fingerprint, so loading the same file
 * again reuses the previously built mesh instead of re-reading and re-parsing
 * it. The cache groups meshes by load options, because different options can
 * produce different scaled meshes from the same source file.
 *
 * @note The cache is not thread-safe; concurrent loads through the same
 *       loader instance must be avoided.
 */
class VN_MESHIO_API MeshLoader
{
    // 类型声明区块
  public:
    /** @brief Length unit of a model's source coordinates. */
    enum class LengthUnit
    {
        /// Meter.
        Meter = 1,
        /// Millimeter.
        Millimeter = 1000,
    };

    /** @brief Vertex scaling strategy. */
    enum class ScaleMode
    {
        /// No scaling is applied.
        Disabled,
        /// The source unit is inferred from the model's AABB diagonal length.
        Auto,
        /// A custom factor scales the vertices.
        Custom,
    };

    /** @brief Load options. */
    struct Options
    {
        /// Vertex scaling mode; defaults to Auto.
        ScaleMode scale_mode{ ScaleMode::Auto };

        /// Target unit used when Auto scaling is active.
        LengthUnit auto_scale_output_unit{ LengthUnit::Millimeter };

        /// Custom scale factor; only used in Custom mode.
        double custom_scale_factor{ 1.0 };

        /**
         * @brief Compares two option sets for equality.
         *
         * @param rhs The option set to compare with.
         * @return true when every field matches.
         */
        bool operator==(const Options& rhs) const noexcept
        {
            return scale_mode == rhs.scale_mode && auto_scale_output_unit == rhs.auto_scale_output_unit
                   && custom_scale_factor == rhs.custom_scale_factor;
        }
    };

    /** @brief Hash functor for Options. */
    struct OptionsHash
    {
        /**
         * @brief Computes a hash of the options.
         *
         * @param options The option set.
         * @return The hash value.
         */
        std::size_t operator()(const Options& options) const noexcept
        {
            std::size_t seed = std::hash<int>{}(static_cast<int>(options.scale_mode));
            seed ^= std::hash<int>{}(static_cast<int>(options.auto_scale_output_unit)) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= std::hash<double>{}(options.custom_scale_factor) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

    // 构造函数区块
  public:
    MeshLoader();
    MeshLoader(const MeshLoader&) = delete;
    MeshLoader(MeshLoader&&) = delete;
    ~MeshLoader();

    // 方法区块
  public:
    /**
     * @brief Returns the shared default loader instance.
     *
     * @return The singleton instance.
     */
    static MeshLoader& defaultInstance();

    /**
     * @brief Checks whether a file is a supported mesh format.
     *
     * @param file_path The model file path.
     * @return true when the file extension is supported.
     */
    static bool isSupportedFormat(const std::filesystem::path& file_path);

    /**
     * @brief Returns the load options.
     *
     * @return Mutable reference to the options.
     */
    Options& options() noexcept;

    /**
     * @brief Sets the load options.
     *
     * @param options The new options.
     */
    void setOptions(const Options& options);

    /**
     * @brief Loads a mesh model from a file.
     *
     * @param file_path The model file path (STL, OBJ, ...).
     * @return The loaded mesh, or null on failure.
     */
    vn::intrusive_ptr<vn::geometry::Mesh> load(const std::filesystem::path& file_path);

    /**
     * @brief Loads a mesh model from a stream.
     *
     * assimp parses a memory image rather than a stream, so the stream is drained first: this entry trades the temporary
     * file for one copy of the model in memory, and it is how a model that lives in a package is loaded without touching
     * the file system at all (wrap a VfsEntrySource in a vn::io::DataSourceStream to read one). The result is cached by
     * content fingerprint exactly like the path entry, so the same model loaded either way is parsed once.
     *
     * @param in The stream to read; it is read to its end.
     * @param format_hint The format to parse as (assimp's format id, e.g. "stl" or "obj"); required, because an
     *        in-memory image carries no file name to infer the format from.
     * @return The loaded mesh, or null when the stream is empty, broke, or held content assimp cannot read.
     */
    vn::intrusive_ptr<vn::geometry::Mesh> load(std::istream& in, const char* format_hint);

    /**
     * @brief Loads a whole model from a file, keeping its structure.
     *
     * Where load() flattens everything into one mesh, this entry keeps what the source described: the node tree with
     * its transforms, one entry per mesh, and the material each mesh uses. The meshes stay in their own local space
     * (nothing is baked), and texture coordinates are kept, because a caller that asked for the structure wants what
     * the file held.
     *
     * Parent directories are irrelevant here; nothing is written. Like load(), the result is cached by content
     * fingerprint, so the same file read through either entry is parsed once per entry.
     *
     * @param file_path The model file path (STL, OBJ, glTF, ...).
     * @return The model, or null when the file is missing or held content assimp cannot read.
     */
    vn::intrusive_ptr<vn::geometry::Model> loadModel(const std::filesystem::path& file_path);

    /**
     * @brief Loads a whole model from a stream, keeping its structure.
     *
     * The stream is drained first, exactly like load(): assimp parses a memory image rather than a stream, so this is
     * the entry that reads a model out of a package without touching the file system.
     *
     * @param in The stream to read; it is read to its end.
     * @param format_hint The format to parse as (assimp's format id, e.g. "stl" or "obj"); required, because an
     *        in-memory image carries no file name to infer the format from.
     * @return The model, or null when the stream is empty, broke, or held content assimp cannot read.
     */
    vn::intrusive_ptr<vn::geometry::Model> loadModel(std::istream& in, const char* format_hint);

    // 方法区块
  private:
    /**
     * @brief Parses the bytes of a model, sharing the cache with the path entry.
     *
     * The counterpart of the path entry: what that one reads from a file, this one is handed as the bytes the file was
     * read into, so a model that lives in a package is parsed the same way as one that lives on disk.
     *
     * @param bytes The bytes of the model; they stay alive for the duration of the call.
     * @param format_hint The format to parse as.
     * @return The loaded mesh, or null on failure.
     */
    vn::intrusive_ptr<vn::geometry::Mesh> loadBytes(std::vector<unsigned char>& bytes, const char* format_hint);

    /**
     * @brief Parses a whole model from the bytes of a model file, sharing the cache with the path entry.
     *
     * @param bytes The bytes of the model; they stay alive for the duration of the call.
     * @param format_hint The format to parse as.
     * @return The model, or null on failure.
     */
    vn::intrusive_ptr<vn::geometry::Model> loadModelBytes(std::vector<unsigned char>& bytes, const char* format_hint);

    /**
     * @brief Hands a built model to the cache, and back to the caller.
     *
     * @param fingerprint The content fingerprint; an empty one is not cached.
     * @param model The model to remember; it is returned unchanged.
     * @return The model the caller receives.
     */
    vn::intrusive_ptr<vn::geometry::Model> rememberModel(const vn::crypto::ByteSequenceFingerprint& fingerprint,
                                                         vn::intrusive_ptr<vn::geometry::Model>        model);

    // 类型声明区块
  private:
    /** @brief Per-file cached meshes and models, grouped by load options. */
    struct CacheData
    {
        std::unordered_map<Options, vn::intrusive_ptr<vn::geometry::Mesh>, OptionsHash>  option_shape_map;
        std::unordered_map<Options, vn::intrusive_ptr<vn::geometry::Model>, OptionsHash> option_model_map;
    };

    // 字段区块
  private:
    Options options_;
    vn::runtime::InMemoryCache<vn::crypto::ByteSequenceFingerprint, CacheData> cache_;
};

VN_MESHIO_NS_END
