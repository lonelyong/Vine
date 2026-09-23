#pragma once

#include <filesystem>
#include <memory>

#include <vine/io/Vfs.hpp>
#include <vine/robotics/io/robot_io_global.hpp>
#include <vine/robotics/io/XmlIOBase.hpp>
#include <vine/robotics/workcell/Device.hpp>
#include <vine/String.hpp>

namespace tinyxml2
{
class XMLDocument;
} // namespace tinyxml2

V_ROBOTICS_IO_NS_BEGIN

/**
 * @brief Serializes and deserializes devices as XML.
 *
 * The root element is <device>. Loads create the concrete device type from
 * the kind attribute: Scanner -> Scanner, anything else -> MotionDevice.
 *
 * Instances are stateless: all per-operation state lives in the parse/export
 * context, so a DeviceIO object is reentrant and safe to reuse, including
 * from multiple threads (each operation carries its own context).
 */
class V_ROBOTICS_IO_API DeviceIO : public XmlIOBase
{
  public:
    /**
     * @brief Load options.
     */
    struct LoadOptions
    {
    };

    /**
     * @brief Save options.
     */
    struct SaveOptions
    {
    };

  public:
    /**
     * @brief Loads a device from an unpacked folder.
     *
     * Reads a .vdev XML file; mesh bins referenced by the XML are resolved
     * relative to the file's directory. No packaging is involved: this is
     * the loader for loose, unpacked resources.
     *
     * @param file_path The .vdev file path.
     * @param options Load options.
     * @return The device, owned by the caller.
     * @throws std::runtime_error on failure.
     */
    std::unique_ptr<workcell::Device> loadXml(const std::filesystem::path& file_path,
                                              const LoadOptions&           options = {});

    /**
     * @brief Loads a device from a device package (.vdevpkg).
     *
     * The package is a zip holding a device.xml (and optional geoms bins);
     * it is opened in memory, never extracted to disk.
     *
     * @param pkg_path The .vdevpkg file path.
     * @param options Load options.
     * @return The device, owned by the caller.
     * @throws std::runtime_error on failure.
     */
    std::unique_ptr<workcell::Device> loadPkg(const std::filesystem::path& pkg_path,
                                              const LoadOptions&           options = {});

    /**
     * @brief Loads a device from a device package held in a VFS.
     *
     * Reads device.xml (and its geoms bins) from the VFS root.
     *
     * @param vfs The virtual file system holding the device resources.
     * @param options Load options.
     * @return The device, owned by the caller.
     * @throws std::runtime_error on failure.
     */
    std::unique_ptr<workcell::Device> loadPkg(vine::io::Vfs& vfs, const LoadOptions& options = {});

    /**
     * @brief Exports a device as a device package (.vdevpkg).
     *
     * The package is a zip holding a device.xml (and optional geoms bins),
     * written directly, without any temporary directory.
     *
     * @param dev The device to save.
     * @param pkg_path The output .vdevpkg file path.
     * @param options Save options.
     * @throws std::runtime_error on failure.
     */
    void savePkg(const workcell::Device& dev, const std::filesystem::path& pkg_path,
                 const SaveOptions& options = {});

    /**
     * @brief Exports a device's resources into a VFS.
     *
     * Writes device.xml (and its geoms bins) into the VFS root.
     *
     * @param dev The device to save.
     * @param vfs The virtual file system to hold the device resources.
     * @param options Save options.
     * @throws std::runtime_error on failure.
     */
    void savePkg(const workcell::Device& dev, vine::io::Vfs& vfs, const SaveOptions& options = {});

    /**
     * @brief Loads a device from a .vdev XML entry inside a VFS.
     *
     * Internal: shared by the folder loader (DirectoryVfs) and WorkcellIO,
     * which loads loose device files from a workcell's folder/package tree.
     * Not one of the public file/pkg serialization entry points.
     *
     * @param vfs The virtual file system.
     * @param vfs_path The virtual path of the .vdev file.
     * @param options Load options.
     * @return The device, owned by the caller.
     */
    std::unique_ptr<workcell::Device> loadXmlFromVfs(vine::io::Vfs& vfs, const String& vfs_path,
                                                     const LoadOptions& options = {});

  private:
    /**
     * @brief Builds a <device> XML document.
     *
     * @param dev The device.
     * @param ctx Export context (may carry a package VFS for mesh bins).
     * @return The XML document, owned by the caller.
     */
    std::unique_ptr<tinyxml2::XMLDocument> buildDoc(const workcell::Device& dev, ExportContext& ctx);

    /**
     * @brief Parses device XML into a device.
     *
     * @param xml_str The XML content.
     * @param ctx Parse context (may carry a package VFS for mesh bins).
     * @return The device, owned by the caller.
     */
    std::unique_ptr<workcell::Device> parseDoc(const String& xml_str, ParseContext& ctx);

    /**
     * @brief Parses the <device> element into a device.
     *
     * @param ctx Parse context.
     * @param xe_device The device element.
     * @return The device, owned by the caller.
     */
    std::unique_ptr<workcell::Device> parseDeviceInternal(ParseContext& ctx, tinyxml2::XMLElement* xe_device);

    /**
     * @brief Writes a device into a <device> element.
     *
     * @param ctx Export context.
     * @param dev The device.
     * @param xe_device The element to fill.
     */
    void exportDeviceInternal(ExportContext& ctx, const workcell::Device& dev, tinyxml2::XMLElement* xe_device);
};

V_ROBOTICS_IO_NS_END
