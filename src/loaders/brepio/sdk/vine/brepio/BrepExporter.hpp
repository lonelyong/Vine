#pragma once

#include "brepio_global.hpp"

#include <filesystem>
#include <ostream>

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/geometry/BrepShape.hpp>

VN_BREPIO_NS_BEGIN

/**
 * @brief Abstract exporter that writes a B-rep solid to a file.
 *
 * Concrete B-rep exporters (STEP, IGES, ...) derive from this class and
 * write a BrepShape into the target format.
 */
class VN_BREPIO_API BrepExporter : public vn::Object, public vn::RefCounted<BrepExporter> {
    VN_OBJECT_META_DECL;

  public:
    BrepExporter();
    ~BrepExporter() override;

  public:
    /**
     * @brief Writes a B-rep solid to a file.
     *
     * @param path  File to write.
     * @param shape B-rep solid to export.
     * @return true on success.
     */
    virtual bool save(const std::filesystem::path& path, const vn::geometry::BrepShape& shape) = 0;

    /**
     * @brief Writes a B-rep solid to a stream.
     *
     * The stream entry sits next to the file entry so that every exporter answers both targets: a caller whose output
     * is a package, a socket or a test needs no temporary file. OpenCASCADE's writers take a stream
     * (STEPControl_Writer::WriteStream, IGESControl_Writer::Write), so an exporter that can write a file can write a
     * stream; an exporter that cannot must say so and return false.
     *
     * @param out   Stream to write to.
     * @param shape B-rep solid to export.
     * @return true on success.
     */
    virtual bool save(std::ostream& out, const vn::geometry::BrepShape& shape) = 0;

    /**
     * @brief Returns whether this exporter supports the given solid.
     *
     * @param shape B-rep solid to check.
     * @return true when the solid is supported.
     */
    virtual bool canExport(const vn::geometry::BrepShape& shape) const = 0;
};

VN_BREPIO_NS_END
