#pragma once

#include "meshio_global.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <unordered_map>

#include <vine/intrusive_ptr.hpp>
#include <vine/crypto/ByteSequenceFingerprint.hpp>
#include <vine/geometry/Mesh.hpp>
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

    // 类型声明区块
  private:
    /** @brief Per-file cached meshes, grouped by load options. */
    struct CacheData
    {
        std::unordered_map<Options, vn::intrusive_ptr<vn::geometry::Mesh>, OptionsHash> option_shape_map;
    };

    // 字段区块
  private:
    Options options_;
    vn::runtime::InMemoryCache<vn::crypto::ByteSequenceFingerprint, CacheData> cache_;
};

VN_MESHIO_NS_END
