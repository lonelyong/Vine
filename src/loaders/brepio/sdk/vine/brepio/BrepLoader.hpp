#pragma once

#include "brepio_global.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <istream>
#include <unordered_map>

#include <vine/intrusive_ptr.hpp>
#include <vine/crypto/ByteSequenceFingerprint.hpp>
#include <vine/geometry/BrepShape.hpp>
#include <vine/runtime/InMemoryCache.hpp>

VN_BREPIO_NS_BEGIN

/**
 * @brief Utility class for loading boundary-representation models (STEP, IGES).
 *
 * Loaded B-rep solids are cached by content fingerprint, so loading the same
 * file again reuses the previously built solid instead of re-parsing it. The
 * cache groups solids by load options.
 *
 * @note The cache is not thread-safe; concurrent loads through the same
 *       loader instance must be avoided.
 */
class VN_BREPIO_API BrepLoader
{
    // 类型声明区块
  public:
    /** @brief Load options. */
    struct Options
    {
        /// Placeholder option, reserved for future use.
        char placeholder{};

        /**
         * @brief Compares two option sets for equality.
         *
         * @param rhs The option set to compare with.
         * @return true when every field matches.
         */
        bool operator==(const Options& rhs) const noexcept
        {
            return placeholder == rhs.placeholder;
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
            return std::hash<char>{}(options.placeholder);
        }
    };

    // 构造函数区块
  public:
    BrepLoader();
    BrepLoader(const BrepLoader&) = delete;
    BrepLoader(BrepLoader&&) = delete;
    ~BrepLoader();

    // 方法区块
  public:
    /**
     * @brief Returns the shared default loader instance.
     *
     * @return The singleton instance.
     */
    static BrepLoader& defaultInstance();

    /**
     * @brief Checks whether a file is a supported B-rep format.
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
     * @brief Returns the load options.
     *
     * @return Const reference to the options.
     */
    const Options& options() const noexcept;

    /**
     * @brief Sets the load options.
     *
     * @param options The new options.
     */
    void setOptions(const Options& options);

    /**
     * @brief Loads a B-rep solid from a file.
     *
     * @param file_path The model file path (STEP, IGES).
     * @return The loaded solid, or null on failure.
     */
    vn::intrusive_ptr<vn::geometry::BrepShape> load(const std::filesystem::path& file_path);

    /**
     * @brief Loads a B-rep solid from a stream.
     *
     * The stream entry mirrors the mesh loader's: a model that lives in a package (a VFS entry, a memory buffer) is
     * loaded without writing a temporary file first. OpenCASCADE's readers take a stream directly
     * (STEPControl_Reader::ReadStream), so unlike the mesh loader nothing has to be drained up front.
     *
     * @note Not wired in yet: no OpenCASCADE backend is linked, so this returns null exactly like the path entry.
     *
     * @param in The stream to read; the reader consumes as much of it as it needs.
     * @param format_hint The model format, given as a file extension such as ".step" or ".stp"; required, because a
     *        stream carries no file name to infer the format from.
     * @return The loaded solid, or null on failure.
     */
    vn::intrusive_ptr<vn::geometry::BrepShape> load(std::istream& in, const char* format_hint);

    // 类型声明区块
  private:
    /** @brief Per-file cached solids, grouped by load options. */
    struct CacheData
    {
        std::unordered_map<Options, vn::intrusive_ptr<vn::geometry::BrepShape>, OptionsHash> option_shape_map;
    };

    // 字段区块
  private:
    Options options_;
    vn::runtime::InMemoryCache<vn::crypto::ByteSequenceFingerprint, CacheData> cache_;
};

VN_BREPIO_NS_END
