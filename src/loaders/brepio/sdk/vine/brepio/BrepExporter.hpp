#pragma once

#include "brepio_global.hpp"

#include <filesystem>

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/geometry/BrepShape.hpp>

V_BREPIO_NS_BEGIN

/**
 * @brief Abstract exporter that writes a B-rep solid to a file.
 *
 * Concrete B-rep exporters (STEP, IGES, ...) derive from this class and
 * write a BrepShape into the target format.
 */
class V_BREPIO_API BrepExporter : public vine::Object, public vine::RefCounted<BrepExporter> {
    V_OBJECT_META_DECL;

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
    virtual bool save(const std::filesystem::path& path, const vine::geometry::BrepShape& shape) = 0;

    /**
     * @brief Returns whether this exporter supports the given solid.
     *
     * @param shape B-rep solid to check.
     * @return true when the solid is supported.
     */
    virtual bool canExport(const vine::geometry::BrepShape& shape) const = 0;
};

V_BREPIO_NS_END
