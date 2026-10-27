#pragma once

#include "brepio_global.hpp"

#include <filesystem>
#include <istream>
#include <set>

#include <vine/String.hpp>
#include <vine/crypto/ByteSequenceFingerprint.hpp>
#include <vine/geometry/BrepShape.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/io/IoError.hpp>
#include <vine/runtime/InMemoryCache.hpp>

#include "BrepFormat.hpp"

VN_BREPIO_NS_BEGIN

/**
 * @brief Loads boundary-representation models (STEP, IGES, B-rep).
 *
 * @details
 * - Both entries answer a vn::io::Result: the solid on success, the failure class otherwise.
 * - A build without the OpenCASCADE backend keeps this contract but cannot open anything: supportedExtensions() is
 *   empty and every load answers IoError::Unsupported. formatFromPath() still names the format - the format is a
 *   fact, the ability to read it is not.
 * - Loaded solids are cached by content fingerprint, so loading the same file again reuses what was built before.
 *
 * @warning The cache is not thread-safe: one loader instance must not be used from two threads at the same time.
 */
class VN_BREPIO_API BrepLoader
{
  public:
    BrepLoader();
    BrepLoader(const BrepLoader&) = delete;
    BrepLoader(BrepLoader&&) = delete;
    ~BrepLoader();

  public:
    /**
     * @brief Returns the file extensions this loader can read.
     *
     * The single source of truth for "what can be opened": generated from the format table, and empty when the build
     * has no backend, because a file picker must not offer a STEP file this loader would refuse.
     *
     * @return The supported extension set (e.g. {".stp", ".step", ...}), empty without a backend.
     */
    static const std::set<vn::String>& supportedExtensions();

    /**
     * @brief Drops every cached solid.
     *
     * @details What callers already hold stays valid: this releases the cache's share, not theirs.
     */
    void clearCache();

    /**
     * @brief Loads a B-rep solid from a file.
     *
     * @param file_path The model file path; its extension selects the format.
     * @return The solid, or the failure class: Unsupported when the extension names no format or this build has no
     *         backend, NotFound when the file is not there, IoFailure when it cannot be read, InvalidData when it cannot
     *         be parsed.
     */
    [[nodiscard]] vn::io::Result<vn::intrusive_ptr<vn::geometry::BrepShape>> load(
        const std::filesystem::path& file_path);

    /**
     * @brief Loads a B-rep solid from a stream.
     *
     * A stream carries no file name, so the format is stated rather than inferred. Unlike the mesh loader there is no
     * guessing: the readers are selected by the format, they do not sniff the content.
     *
     * @param in     The stream to read; it is read to its end.
     * @param format The format to parse as; BrepFormat::Unknown is refused with IoError::Unsupported.
     * @return The solid, or the failure class (see the path entry).
     */
    [[nodiscard]] vn::io::Result<vn::intrusive_ptr<vn::geometry::BrepShape>> load(std::istream& in, BrepFormat format);

  private:
    /// Solids already built, keyed by the content fingerprint of the file they came from.
    vn::runtime::InMemoryCache<vn::crypto::ByteSequenceFingerprint, vn::intrusive_ptr<vn::geometry::BrepShape>> cache_;
};

VN_BREPIO_NS_END
