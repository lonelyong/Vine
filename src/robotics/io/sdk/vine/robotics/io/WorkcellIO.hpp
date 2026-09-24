#pragma once

#include <filesystem>
#include <memory>

#include <vine/io/Vfs.hpp>
#include <vine/robotics/io/robot_io_global.hpp>
#include <vine/robotics/io/XmlIOBase.hpp>
#include <vine/robotics/workcell/Device.hpp>
#include <vine/robotics/workcell/RigidObject.hpp>
#include <vine/robotics/workcell/SceneObject.hpp>
#include <vine/robotics/workcell/Workcell.hpp>
#include <vine/String.hpp>

namespace tinyxml2
{
class XMLDocument;
class XMLElement;
} // namespace tinyxml2

VN_ROBOTICS_IO_NS_BEGIN

/**
 * @brief Serializes and deserializes a workcell as XML.
 *
 * The root element is <workcell>. Devices are stored as nested .vdevpkg
 * packages under a "devices" directory and referenced by a relative file
 * attribute; rigid objects are inlined with their visuals and collisions.
 *
 * Instances are stateless: all per-operation state (the workcell being
 * filled, the active VFS and the document directory) lives in the
 * parse/export context, so a WorkcellIO object is reentrant and safe to
 * reuse, including from multiple threads (each operation carries its own
 * context).
 */
class VN_ROBOTICS_IO_API WorkcellIO : public XmlIOBase
{
  public:
    /**
     * @brief Destroys the workcell IO.
     */
    ~WorkcellIO() override;

    /**
     * @brief Save options.
     */
    struct SaveOptions
    {
    };

  public:
    /**
     * @brief Loads a workcell from an unpacked folder.
     *
     * Reads a .vcell XML file; device files are loaded from the "devices"
     * subdirectory and mesh bins are resolved relative to the folder. No
     * packaging is involved: this is the loader for loose, unpacked resources.
     *
     * @param file_path The .vcell file path.
     * @return The workcell, owned by the caller.
     * @throws std::runtime_error on failure.
     */
    std::unique_ptr<workcell::Workcell> loadXml(const std::filesystem::path& file_path);

    /**
     * @brief Loads a workcell from a workcell package (.vwspkg).
     *
     * The package is a zip holding a workcell.xml, nested device packages
     * and optional geoms bins; it is opened in memory and never extracted to
     * disk.
     *
     * @param pkg_path The .vwspkg file path.
     * @return The workcell, owned by the caller.
     * @throws std::runtime_error on failure.
     */
    std::unique_ptr<workcell::Workcell> loadPkg(const std::filesystem::path& pkg_path);

    /**
     * @brief Loads a workcell from a workcell package held in a VFS.
     *
     * Reads workcell.xml from the VFS root, then the referenced device
     * packages (*.vdevpkg) and geoms bins from the same VFS.
     *
     * @param vfs The virtual file system holding the workcell resources.
     * @return The workcell, owned by the caller.
     * @throws std::runtime_error on failure.
     */
    std::unique_ptr<workcell::Workcell> loadPkg(vn::io::Vfs& vfs);

    /**
     * @brief Exports a workcell as a workcell package (.vwspkg).
     *
     * The package is a zip holding a workcell.xml, nested device .vdevpkg
     * files and optional geoms bins, written directly without any temporary
     * directory.
     *
     * @param cell The workcell.
     * @param pkg_path The output .vwspkg file path.
     * @param options Save options.
     * @throws std::runtime_error on failure.
     */
    void savePkg(const workcell::Workcell& cell, const std::filesystem::path& pkg_path,
                 const SaveOptions& options = {});

    /**
     * @brief Exports a workcell's resources into a VFS.
     *
     * Writes workcell.xml into the VFS root, plus the nested device .vdevpkg
     * files under devices/ and the geoms bins.
     *
     * @param cell The workcell.
     * @param vfs The virtual file system to hold the workcell resources.
     * @param options Save options.
     * @throws std::runtime_error on failure.
     */
    void savePkg(const workcell::Workcell& cell, vn::io::Vfs& vfs, const SaveOptions& options = {});

  private:
    /**
     * @brief Shared load body: reads the .vcell and referenced entries from a VFS.
     *
     * @param vfs The virtual file system (DirectoryVfs for folders, ZipArchive for packages).
     * @param vfs_path The virtual path of the .vcell file.
     * @return The workcell, owned by the caller.
     */
    std::unique_ptr<workcell::Workcell> loadVfs(vn::io::Vfs& vfs, const std::filesystem::path& vfs_path);

    /**
     * @brief Shared export body: writes the .vcell and device files into a VFS.
     *
     * @param cell The workcell.
     * @param vfs The virtual file system.
     * @param vfs_path The virtual path of the .vcell file.
     */
    void exportToVfs(const workcell::Workcell& cell, vn::io::Vfs& vfs, const std::filesystem::path& vfs_path);

    /**
     * @brief Exports one object (and its children) as an <obj> element.
     *
     * @param ctx Export context.
     * @param obj The object.
     * @param xe_parent The element to append the <obj> to.
     */
    void exportObject(ExportContext& ctx, const workcell::SceneObject& obj, tinyxml2::XMLElement* xe_parent);

    /**
     * @brief Writes the device-specific <obj> attributes and the device file.
     *
     * @param ctx Export context.
     * @param dev The device.
     * @param xe The <obj> element.
     */
    void exportDevice(ExportContext& ctx, const workcell::Device& dev, tinyxml2::XMLElement* xe);

    /**
     * @brief Writes the rigid-object body (visuals / collisions) inlined.
     *
     * @param ctx Export context.
     * @param obj The rigid object.
     * @param xe The <obj> element.
     */
    void exportRigidObject(ExportContext& ctx, const workcell::RigidObject& obj, tinyxml2::XMLElement* xe);

    /**
     * @brief Parses one <obj> element (and its children).
     *
     * @param ctx Parse context.
     * @param parent The parent object, or null for a top-level object.
     * @param xe The <obj> element.
     */
    void parseObject(ParseContext& ctx, raw_ptr<workcell::SceneObject> parent, tinyxml2::XMLElement* xe);

    /**
     * @brief Parses the common <obj> attributes (name / origin / parent frame).
     *
     * @param ctx Parse context.
     * @param obj The object.
     * @param xe The <obj> element.
     */
    void parseObjectCommon(ParseContext& ctx, workcell::SceneObject& obj, tinyxml2::XMLElement* xe);

    /**
     * @brief Loads a device from its referenced file.
     *
     * @param ctx Parse context.
     * @param xe The <obj> element.
     * @return The device as a scene object, owned by the caller.
     */
    std::unique_ptr<workcell::SceneObject> parseDevice(ParseContext& ctx, tinyxml2::XMLElement* xe);

    /**
     * @brief Parses a rigid object's body (visuals / collisions).
     *
     * @param ctx Parse context.
     * @param obj The rigid object.
     * @param xe The <obj> element.
     */
    void parseRigidObject(ParseContext& ctx, workcell::RigidObject& obj, tinyxml2::XMLElement* xe);
};

VN_ROBOTICS_IO_NS_END
