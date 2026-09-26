#include <vine/robotics/io/WorkcellIO.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <tinyxml2.h>

#include <vine/io/DirectoryVfs.hpp>
#include <vine/io/Vfs.hpp>
#include <vine/io/ZipArchive.hpp>
#include <vine/robotics/io/DeviceIO.hpp>
#include <vine/robotics/io/robot_io_global.hpp>
#include <vine/robotics/workcell/MotionDevice.hpp>
#include <vine/robotics/workcell/RigidObject.hpp>

#include "IoUtils.hpp"

VN_ROBOTICS_IO_NS_BEGIN

WorkcellIO::~WorkcellIO() = default;

namespace
{

/**
 * @brief Converts a String to a C string.
 *
 * @param s The string.
 * @return The UTF-8 byte pointer.
 */
const char* toCStr(const String& s)
{
    return reinterpret_cast<const char*>(s.c_str());
}

/**
 * @brief Returns an attribute value as a String.
 *
 * @param xe The element.
 * @param name The attribute name.
 * @return The value, or empty.
 */
String attr(const tinyxml2::XMLElement* xe, const char* name)
{
    const char* const value = xe->Attribute(name);
    return value ? String::fromUtf8(value) : String{};
}

} // namespace

std::unique_ptr<workcell::Workcell> WorkcellIO::loadXml(const std::filesystem::path& file_path)
{
    std::filesystem::path dir = file_path.parent_path();
    if (dir.empty()) {
        dir = std::filesystem::path(".");
    }
    vn::io::DirectoryVfs vfs(dir);
    return loadVfs(vfs, detail::pathLeafName(file_path));
}

std::unique_ptr<workcell::Workcell> WorkcellIO::loadVfs(vn::io::Vfs& vfs, const std::filesystem::path& vfs_path)
{
    const auto xml = detail::readText(vfs, vfs_path);
    if (!xml) {
        throw std::runtime_error("WorkcellIO::loadXml, failed to read vfs file: " + vfs_path.generic_string());
    }

    ParseOptions opts;
    ParseContext ctx(opts);
    ctx.vfs = &vfs;
    tinyxml2::XMLDocument doc;
    doc.Parse(toCStr(xml.value()));
    if (doc.Error()) {
        throw std::runtime_error(std::string("WorkcellIO::loadXml, failed to parse workcell xml: ") + doc.ErrorStr());
    }
    auto* const xe_cell = doc.FirstChildElement("workcell");
    if (xe_cell == nullptr) {
        throw std::runtime_error("WorkcellIO::loadXml, no <workcell> root element found.");
    }

    uint16_t major = 0, minor = 0;
    parseVersion(ctx, major, minor, xe_cell);
    if (major != VN_ROBOTICS_IO_VERSION_MAJOR || minor != VN_ROBOTICS_IO_VERSION_MINOR) {
        throw std::runtime_error("WorkcellIO::loadXml, unsupported version: " + std::to_string(major) + "."
                                 + std::to_string(minor));
    }

    ctx.vfs_dir = detail::vfsParentDir(vfs_path);

    auto cell = std::make_unique<workcell::Workcell>();
    cell->setName(attr(xe_cell, "name"));
    ctx.cell = cell.get();

    for (auto* xe_obj = xe_cell->FirstChildElement("obj"); xe_obj; xe_obj = xe_obj->NextSiblingElement("obj")) {
        parseObject(ctx, nullptr, xe_obj);
    }
    return cell;
}

void WorkcellIO::exportToVfs(const workcell::Workcell& cell, vn::io::Vfs& vfs, const std::filesystem::path& vfs_path)
{
    ExportOptions opts;
    ExportContext ctx(opts);
    ctx.vfs     = &vfs;
    ctx.vfs_dir = detail::vfsParentDir(vfs_path);
    // Export only reads the workcell (frame lookups), so the cell stays const.
    ctx.cell = &cell;

    auto        doc     = std::make_unique<tinyxml2::XMLDocument>();
    auto* const xe_cell = doc->NewElement("workcell");
    xe_cell->SetAttribute("name", toCStr(cell.name()));
    exportVersion(ctx, VN_ROBOTICS_IO_VERSION_MAJOR, VN_ROBOTICS_IO_VERSION_MINOR, xe_cell);
    doc->InsertEndChild(xe_cell);

    for (const auto* const obj : cell.sceneObjects()) {
        if (obj->parentObject() == nullptr) {
            exportObject(ctx, *obj, xe_cell);
        }
    }

    tinyxml2::XMLPrinter printer;
    doc->Print(&printer);
    // CStrSize() counts the terminating NUL the printer appends, and the document is the text without it.
    const String xml = String::fromUtf8(std::string_view(printer.CStr(), printer.CStrSize() - 1));
    if (detail::writeText(vfs, vfs_path, xml) != vn::io::IoError::Ok) {
        throw std::runtime_error("WorkcellIO::savePkg, failed to write vfs file: " + vfs_path.generic_string());
    }
}

void WorkcellIO::savePkg(const workcell::Workcell& cell, vn::io::Vfs& vfs, const SaveOptions& options)
{
    (void)options;
    exportToVfs(cell, vfs, std::filesystem::path(u8"workcell.xml"));
}

std::unique_ptr<workcell::Workcell> WorkcellIO::loadPkg(const std::filesystem::path& pkg_path)
{
    // Only the archive index is read up front; entries decompress on demand.
    auto pkg = vn::io::ZipArchive::open(pkg_path, vn::io::ZipArchive::OpenMode::ReadOnly);
    if (!pkg) {
        throw std::runtime_error("WorkcellIO::loadPkg, not a valid workcell package: " + pkg_path.string());
    }
    return loadPkg(*pkg);
}

std::unique_ptr<workcell::Workcell> WorkcellIO::loadPkg(vn::io::Vfs& vfs)
{
    return loadVfs(vfs, std::filesystem::path(u8"workcell.xml"));
}

void WorkcellIO::savePkg(const workcell::Workcell& cell, const std::filesystem::path& pkg_path,
                         const SaveOptions& options)
{
    vn::io::ZipArchive vfs;
    savePkg(cell, vfs, options);
    if (vfs.saveAs(pkg_path) != vn::io::IoError::Ok) {
        throw std::runtime_error("WorkcellIO::savePkg, failed to write package file: " + pkg_path.string());
    }
}

void WorkcellIO::exportObject(ExportContext& ctx, const workcell::SceneObject& obj, tinyxml2::XMLElement* xe_parent)
{
    auto xe_obj = xe_parent->GetDocument()->NewElement("obj");
    xe_obj->SetAttribute("name", toCStr(obj.name()));

    if (obj.kind() == workcell::SceneObjectKind::Device) {
        xe_obj->SetAttribute("type", "device");
        exportDevice(ctx, static_cast<const workcell::Device&>(obj), xe_obj);
    }
    else {
        xe_obj->SetAttribute("type", "rigid_object");
        exportRigidObject(ctx, static_cast<const workcell::RigidObject&>(obj), xe_obj);
    }

    exportPose(ctx, obj.baseFrame()->fixedTransform(), xe_obj);

    const auto parent_frame = obj.baseFrame()->parent();
    if (parent_frame && parent_frame != ctx.cell->worldFrame()) {
        const auto parent_obj = ctx.cell->findSceneObjectByFrame(parent_frame);
        if (parent_obj && parent_frame != parent_obj->baseFrame()) {
            xe_obj->SetAttribute("parent_frame", toCStr(parent_frame->name()));
        }
    }

    const auto children = obj.childObjects();
    if (!children.empty()) {
        auto xe_children = xe_parent->GetDocument()->NewElement("children");
        for (const auto* const child : children) {
            exportObject(ctx, *child, xe_children);
        }
        xe_obj->LinkEndChild(xe_children);
    }
    xe_parent->LinkEndChild(xe_obj);
}

void WorkcellIO::exportDevice(ExportContext& ctx, const workcell::Device& dev, tinyxml2::XMLElement* xe)
{
    const String                 rel      = String(u8"devices/") + dev.name() + String(u8".vdevpkg");
    const std::filesystem::path  dev_path = ctx.vfs_dir.empty() ? detail::vfsPath(rel) : ctx.vfs_dir / detail::vfsPath(rel);
    DeviceIO     device_io;
    // Build the device package in an inner VFS, then store it as one entry.
    vn::io::ZipArchive inner;
    device_io.savePkg(dev, inner);
    const auto zip_bytes = inner.toBytes();
    if (!zip_bytes) {
        throw std::runtime_error("WorkcellIO::exportDevice, failed to build device package: "
                                 + dev.name().as_std_str());
    }
    if (ctx.vfs->addFile(dev_path, zip_bytes.value()) != vn::io::IoError::Ok) {
        throw std::runtime_error("WorkcellIO::exportDevice, failed to write device package: "
                                 + dev_path.generic_string());
    }
    xe->SetAttribute("file", toCStr(rel));

    if (const auto* const motion = dynamic_cast<const workcell::MotionDevice*>(&dev)) {
        if (!motion->homeQ().empty()) {
            xe->SetAttribute("home", toCStr(detail::qToStr(motion->homeQ())));
        }
        if (motion->kinematics()) {
            const auto& resolutions = motion->kinematics()->jointResolutions();
            if (!resolutions.empty()) {
                xe->SetAttribute("resolutions", toCStr(detail::qToStr(resolutions)));
            }
        }
    }
}

void WorkcellIO::exportRigidObject(ExportContext& ctx, const workcell::RigidObject& obj, tinyxml2::XMLElement* xe)
{
    for (const auto& visual : obj.body().visuals()) {
        exportVisual(ctx, visual, xe);
    }
    for (const auto& collision : obj.body().collisions()) {
        exportCollision(ctx, collision, xe);
    }
}

void WorkcellIO::parseObject(ParseContext& ctx, raw_ptr<workcell::SceneObject> parent, tinyxml2::XMLElement* xe)
{
    const String type = attr(xe, "type");
    const String name = attr(xe, "name");
    if (type.empty() || name.empty()) {
        throw std::runtime_error("WorkcellIO::parseObject, <obj> requires type and name attributes.");
    }

    std::unique_ptr<workcell::SceneObject> object;
    if (type == u8"device") {
        object = parseDevice(ctx, xe);
        object->setName(name);
    }
    else if (type == u8"rigid_object") {
        auto rigid = std::make_unique<workcell::RigidObject>(name);
        parseRigidObject(ctx, *rigid, xe);
        object = std::move(rigid);
    }
    else {
        throw std::runtime_error("WorkcellIO::parseObject, unknown object type: " + type.as_std_str());
    }

    parseObjectCommon(ctx, *object, xe);

    // Resolve the parent frame: explicit parent_frame attribute or the parent's base frame.
    raw_ptr<kinematics::Frame> parent_frame = parent ? parent->baseFrame() : nullptr;
    const String               parent_frame_name = attr(xe, "parent_frame");
    if (!parent_frame_name.empty() && parent) {
        raw_ptr<kinematics::Frame> found = nullptr;
        for (const auto frame : parent->frames()) {
            if (frame->name() == parent_frame_name) {
                found = frame;
                break;
            }
        }
        if (found == nullptr) {
            throw std::runtime_error("WorkcellIO::parseObject, parent frame not found: " + parent_frame_name.as_std_str());
        }
        parent_frame = found;
    }

    if (ctx.cell->addSceneObject(std::move(object), parent_frame) == nullptr) {
        throw std::runtime_error("WorkcellIO::parseObject, failed to add object: " + name.as_std_str());
    }

    if (auto* const xe_children = xe->FirstChildElement("children")) {
        const auto added = ctx.cell->findSceneObject(name);
        for (auto* xe_child = xe_children->FirstChildElement("obj"); xe_child;
             xe_child       = xe_child->NextSiblingElement("obj")) {
            parseObject(ctx, added, xe_child);
        }
    }
}

void WorkcellIO::parseObjectCommon(ParseContext& ctx, workcell::SceneObject& obj, tinyxml2::XMLElement* xe)
{
    if (const auto* const xe_origin = xe->FirstChildElement("origin")) {
        math::Isometry3d origin;
        parsePose(ctx, origin, xe_origin);
        obj.setBaseTransform(origin);
    }
}

std::unique_ptr<workcell::SceneObject> WorkcellIO::parseDevice(ParseContext& ctx, tinyxml2::XMLElement* xe)
{
    const String file = attr(xe, "file");
    if (file.empty()) {
        throw std::runtime_error("WorkcellIO::parseDevice, device <obj> requires a file attribute.");
    }
    const std::filesystem::path  dev_path = ctx.vfs_dir.empty() ? detail::vfsPath(file) : ctx.vfs_dir / detail::vfsPath(file);
    DeviceIO     device_io;
    std::unique_ptr<workcell::Device> dev;
    if (detail::endsWith(file, ".vdevpkg")) {
        auto bytes = ctx.vfs->read(dev_path);
        if (!bytes) {
            throw std::runtime_error("WorkcellIO::parseDevice, failed to read device package: "
                                     + dev_path.generic_string());
        }
        // The nested package is already in memory, so it is indexed rather than
        // expanded: only the entries the device actually needs get decompressed.
        auto pkg = vn::io::ZipArchive::open(bytes.take(), vn::io::ZipArchive::OpenMode::ReadOnly);
        if (!pkg) {
            throw std::runtime_error("WorkcellIO::parseDevice, invalid device package: " + dev_path.generic_string());
        }
        dev = device_io.loadPkg(*pkg);
    }
    else {
        dev = device_io.loadXmlFromVfs(*ctx.vfs, dev_path);
    }

    if (auto* const motion = dynamic_cast<workcell::MotionDevice*>(dev.get())) {
        const String home = attr(xe, "home");
        if (!home.empty()) {
            kinematics::Q q;
            if (detail::strToQ(home, q)) {
                motion->setHomeQ(q);
            }
        }
        const String resolutions = attr(xe, "resolutions");
        if (!resolutions.empty() && motion->kinematics()) {
            kinematics::Q q;
            if (detail::strToQ(resolutions, q)) {
                motion->kinematics()->setJointResolutions(q);
            }
        }
    }
    return dev;
}

void WorkcellIO::parseRigidObject(ParseContext& ctx, workcell::RigidObject& obj, tinyxml2::XMLElement* xe)
{
    auto& body = obj.body();
    for (const auto* xe_visual = xe->FirstChildElement("visual"); xe_visual;
         xe_visual              = xe_visual->NextSiblingElement("visual")) {
        workcell::Visual visual;
        parseVisual(ctx, visual, xe_visual);
        body.visuals().push_back(std::move(visual));
    }
    for (const auto* xe_collision = xe->FirstChildElement("collision"); xe_collision;
         xe_collision             = xe_collision->NextSiblingElement("collision")) {
        workcell::Collision collision;
        parseCollision(ctx, collision, xe_collision);
        body.collisions().push_back(std::move(collision));
    }
}

VN_ROBOTICS_IO_NS_END
