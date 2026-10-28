#pragma once

#include "brepio_global.hpp"

#include <filesystem>
#include <memory>
#include <ostream>

#include <vine/geometry/BrepShape.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/io/IoError.hpp>
#include <vine/io/Stream.hpp>

#include "BrepFormat.hpp"

VN_BREPIO_NS_BEGIN

/**
 * @brief Exports B-rep solids (STEP, IGES, B-rep).
 *
 * @details
 * - Every entry answers a vn::io::IoError instead of throwing.
 * - The target decides how many files one write may produce: a path can hold a companion file, a stream or a data
 *   source holds exactly one self-contained file. All three formats this module declares are single-file.
 * - A build without the OpenCASCADE backend keeps the contract but cannot write anything: every export answers
 *   IoError::Unsupported.
 */
class VN_BREPIO_API BrepExporter {
  public:
    BrepExporter();

  public:
    /**
     * @brief Writes a solid to a file.
     *
     * @param shape     The solid to export.
     * @param format    The target format.
     * @param file_path The output file path; missing parent directories are created.
     * @return IoError::Ok on success; Unsupported when the format names no format or this build has no backend,
     *         InvalidData when the solid is not exportable, IoFailure when the file cannot be written.
     */
    [[nodiscard]] vn::io::IoError exportAs(const vn::geometry::BrepShape& shape, BrepFormat format, const std::filesystem::path& file_path) const;

    /**
     * @brief Writes a solid into one stream.
     *
     * @param shape  The solid to export.
     * @param format The target format.
     * @param out    The stream to write; it must outlive the call.
     * @return IoError::Ok on success; Unsupported when the format names no format or this build has no backend,
     *         InvalidData when the solid is not exportable, IoFailure when the stream refuses the bytes.
     */
    [[nodiscard]] vn::io::IoError exportAs(const vn::geometry::BrepShape& shape, BrepFormat format, std::ostream& out) const;

    /**
     * @brief Creates a source that exports a solid on demand.
     *
     * This is how a solid reaches a package (a Vfs entry) without a temporary file; the first pull produces the bytes
     * and rewind() replays them. A build without a backend answers a source whose error() is IoError::Unsupported.
     *
     * @param shape  The solid to export; the source holds it.
     * @param format The target format.
     * @return The source; never null. A refusal surfaces through the source's error(), not here.
     */
    std::shared_ptr<vn::io::DataSource> createSource(vn::intrusive_ptr<const vn::geometry::BrepShape> shape, BrepFormat format) const;
};

VN_BREPIO_NS_END
