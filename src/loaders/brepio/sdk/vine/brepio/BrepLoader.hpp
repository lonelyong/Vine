#pragma once

#include "brepio_global.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <unordered_map>

#include <vine/intrusive_ptr.hpp>
#include <vine/crypto/ByteSequenceFingerprint.hpp>
#include <vine/geometry/BrepShape.hpp>
#include <vine/runtime/InMemoryCache.hpp>

V_BREPIO_NS_BEGIN

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
class V_BREPIO_API BrepLoader
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
    vine::intrusive_ptr<vine::geometry::BrepShape> load(const std::filesystem::path& file_path);

    // 类型声明区块
  private:
    /** @brief Per-file cached solids, grouped by load options. */
    struct CacheData
    {
        std::unordered_map<Options, vine::intrusive_ptr<vine::geometry::BrepShape>, OptionsHash> option_shape_map;
    };

    // 字段区块
  private:
    Options options_;
    vine::runtime::InMemoryCache<vine::crypto::ByteSequenceFingerprint, CacheData> cache_;
};

V_BREPIO_NS_END
