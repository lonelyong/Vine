#include <vine/robotics/io/DeviceIO.hpp>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <tinyxml2.h>

#include <vine/geometry/ColorMaterial.hpp>
#include <vine/io/DirectoryVfs.hpp>
#include <vine/io/Vfs.hpp>
#include <vine/io/ZipArchive.hpp>
#include <vine/robotics/io/robot_io_global.hpp>
#include <vine/robotics/workcell/MotionDevice.hpp>
#include <vine/robotics/workcell/Scanner.hpp>

#include "IoUtils.hpp"

V_ROBOTICS_IO_NS_BEGIN

namespace
{

/**
 * @brief Converts a device kind to its XML string.
 *
 * @param kind The device kind.
 * @return The XML string.
 */
const char* kindToStr(workcell::DeviceKind kind)
{
    switch (kind) {
    case workcell::DeviceKind::Manipulator:
        return "Manipulator";
    case workcell::DeviceKind::ExternalAxis:
        return "ExternalAxis";
    case workcell::DeviceKind::Positioner:
        return "Positioner";
    case workcell::DeviceKind::Scanner:
        return "Scanner";
    case workcell::DeviceKind::Tool:
        return "Tool";
    default:
        return "Other";
    }
}

/**
 * @brief Converts an XML string to a device kind.
 *
 * @param str The XML string.
 * @return The device kind.
 */
workcell::DeviceKind kindFromStr(const String& str)
{
    if (str == u8"Manipulator") {
        return workcell::DeviceKind::Manipulator;
    }
    if (str == u8"ExternalAxis") {
        return workcell::DeviceKind::ExternalAxis;
    }
    if (str == u8"Positioner") {
        return workcell::DeviceKind::Positioner;
    }
    if (str == u8"Scanner") {
        return workcell::DeviceKind::Scanner;
    }
    if (str == u8"Tool") {
        return workcell::DeviceKind::Tool;
    }
    return workcell::DeviceKind::Other;
}

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
 * @brief Reads a double attribute.
 *
 * @param xe The element.
 * @param name The attribute name.
 * @param out Receives the value.
 * @return true when the attribute parsed.
 */
bool getAttrDouble(const tinyxml2::XMLElement* xe, const char* name, double& out)
{
    const char* const value = xe->Attribute(name);
    return value != nullptr && vine::robotics::io::detail::strToDouble(
                                   vine::String(reinterpret_cast<const char8_t*>(value)), out);
}

/**
 * @brief Writes a double attribute.
 *
 * @param xe The element.
 * @param name The attribute name.
 * @param value The value.
 */
void setAttrDouble(tinyxml2::XMLElement* xe, const char* name, double value)
{
    xe->SetAttribute(name, toCStr(vine::robotics::io::detail::doubleToStr(value)));
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
    return value ? String(reinterpret_cast<const char8_t*>(value)) : String();
}

} // namespace

std::unique_ptr<workcell::Device> DeviceIO::loadXml(const std::filesystem::path& file_path, const LoadOptions& options)
{
    std::filesystem::path dir = file_path.parent_path();
    if (dir.empty()) {
        dir = std::filesystem::path(".");
    }
    vine::io::DirectoryVfs vfs(dir);
    auto                   dev = loadXmlFromVfs(vfs, detail::pathLeafName(file_path), options);
    dev->setFilePath(file_path);
    return dev;
}

std::unique_ptr<workcell::Device> DeviceIO::loadXmlFromVfs(vine::io::Vfs& vfs, const String& vfs_path,
                                                           const LoadOptions& options)
{
    (void)options;
    ParseOptions opts;
    ParseContext ctx(opts);
    ctx.vfs = &vfs;
    const auto xml = detail::readText(vfs, vfs_path);
    if (!xml) {
        throw std::runtime_error("DeviceIO::loadXml, failed to read vfs file: " + vfs_path.as_std_str());
    }
    return parseDoc(xml.value(), ctx);
}

std::unique_ptr<workcell::Device> DeviceIO::parseDoc(const String& xml_str, ParseContext& ctx)
{
    tinyxml2::XMLDocument doc;
    doc.Parse(toCStr(xml_str));
    if (doc.Error()) {
        throw std::runtime_error(std::string("DeviceIO::loadXml, failed to parse device xml: ") + doc.ErrorStr());
    }
    auto* const xe_device = doc.FirstChildElement("device");
    if (xe_device == nullptr) {
        throw std::runtime_error("DeviceIO::loadXml, no <device> root element found.");
    }
    return parseDeviceInternal(ctx, xe_device);
}

std::unique_ptr<workcell::Device> DeviceIO::parseDeviceInternal(ParseContext& ctx, tinyxml2::XMLElement* xe_device)
{
    // ---- version ----
    uint16_t major = 0, minor = 0;
    parseVersion(ctx, major, minor, xe_device);
    if (major != V_ROBOTICS_IO_VERSION_MAJOR || minor != V_ROBOTICS_IO_VERSION_MINOR) {
        throw std::runtime_error("DeviceIO::parseDeviceInternal, unsupported version: " + std::to_string(major) + "."
                                 + std::to_string(minor));
    }

    // ---- identity / kind ----
    const String name = attr(xe_device, "name");
    const String kind_str = attr(xe_device, "kind");
    const workcell::DeviceKind kind = kindFromStr(kind_str);

    // ---- metadata (carries the length unit) ----
    workcell::DeviceMetadata meta;
    if (auto* const xe_meta = xe_device->FirstChildElement("metadata")) {
        parseDeviceMetadata(ctx, meta, xe_meta);
    }
    if (meta.name.empty()) {
        meta.name = name;
    }
    // Length scale: file unit -> in-memory mm.
    const double file_value = meta.length_unit == workcell::LengthUnit::Meter
                                  ? static_cast<double>(workcell::LengthUnit::Meter)
                                  : static_cast<double>(workcell::LengthUnit::Millimeter);
    ctx.options.len_unit_scaling = static_cast<double>(workcell::LengthUnit::Millimeter) / file_value;

    // ---- materials (named library, visual references resolve against it) ----
    std::vector<workcell::DeviceMaterial> materials;
    if (auto* const xe_materials = xe_device->FirstChildElement("materials")) {
        for (auto* xe_mat = xe_materials->FirstChildElement("material"); xe_mat;
             xe_mat = xe_mat->NextSiblingElement("material")) {
            const String mat_name = attr(xe_mat, "name");
            if (mat_name.empty()) {
                continue;
            }
            auto material = parseMaterial(ctx, xe_mat);
            if (material) {
                ctx.materials_by_name[mat_name] = material;
                materials.push_back(workcell::DeviceMaterial{ mat_name, material });
            }
        }
    }

    // ---- links ----
    std::vector<std::unique_ptr<workcell::Link>>   links;
    std::map<String, raw_ptr<workcell::Link>>      link_by_name;
    for (auto* xe_link = xe_device->FirstChildElement("link"); xe_link;
         xe_link       = xe_link->NextSiblingElement("link")) {
        auto link = std::make_unique<workcell::Link>(String());
        parseLink(ctx, *link, xe_link);
        link_by_name[link->name()] = link.get();
        links.push_back(std::move(link));
    }
    if (links.empty()) {
        throw std::runtime_error("DeviceIO::parseDeviceInternal, no <link> elements found.");
    }

    // ---- joints ----
    std::vector<std::unique_ptr<workcell::Joint>> joints;
    for (auto* xe_joint = xe_device->FirstChildElement("joint"); xe_joint;
         xe_joint       = xe_joint->NextSiblingElement("joint")) {
        joints.push_back(parseJoint(ctx, xe_joint, link_by_name));
    }

    // ---- device by kind ----
    std::unique_ptr<workcell::Device> dev;
    std::unique_ptr<workcell::DeviceData> data;
    if (kind == workcell::DeviceKind::Scanner) {
        auto sdata = std::make_unique<workcell::ScannerData>();
        for (auto* xe_cam = xe_device->FirstChildElement("camera"); xe_cam;
             xe_cam       = xe_cam->NextSiblingElement("camera")) {
            auto cam  = std::make_unique<workcell::Scanner::Camera>();
            cam->frame_name = attr(xe_cam, "frame");
            for (const char* tag : { "design", "calibrated" }) {
                if (const auto* xe_intr = xe_cam->FirstChildElement(tag)) {
                    auto& intr = std::string(tag) == "design" ? cam->design_intrinsics : cam->calibrated_intrinsics;
                    getAttrDouble(xe_intr, "width", intr.width);
                    getAttrDouble(xe_intr, "height", intr.height);
                    getAttrDouble(xe_intr, "center_x", intr.center_x);
                    getAttrDouble(xe_intr, "center_y", intr.center_y);
                    getAttrDouble(xe_intr, "focus_x", intr.focus_x);
                    getAttrDouble(xe_intr, "focus_y", intr.focus_y);
                    getAttrDouble(xe_intr, "near", intr.near_);
                    getAttrDouble(xe_intr, "far", intr.far_);
                    intr.near_ *= ctx.options.len_unit_scaling;
                    intr.far_ *= ctx.options.len_unit_scaling;
                }
            }
            sdata->cameras.push_back(std::move(cam));
        }
        for (auto* xe_proj = xe_device->FirstChildElement("projector"); xe_proj;
             xe_proj       = xe_proj->NextSiblingElement("projector")) {
            auto proj  = std::make_unique<workcell::Scanner::Projector>();
            proj->frame_name = attr(xe_proj, "frame");
            for (const char* tag : { "design", "calibrated" }) {
                if (const auto* xe_params = xe_proj->FirstChildElement(tag)) {
                    auto& params = std::string(tag) == "design" ? proj->design_params : proj->calibrated_params;
                    getAttrDouble(xe_params, "fov_w", params.fov_w);
                    getAttrDouble(xe_params, "fov_h", params.fov_h);
                    getAttrDouble(xe_params, "min_dof", params.min_dof);
                    getAttrDouble(xe_params, "max_dof", params.max_dof);
                    getAttrDouble(xe_params, "measure_dist", params.measure_dist);
                    getAttrDouble(xe_params, "crop_value", params.crop_value);
                    params.min_dof *= ctx.options.len_unit_scaling;
                    params.max_dof *= ctx.options.len_unit_scaling;
                    params.measure_dist *= ctx.options.len_unit_scaling;
                }
            }
            sdata->projectors.push_back(std::move(proj));
        }
        data = std::move(sdata);
        dev  = std::make_unique<workcell::Scanner>();
    }
    else {
        auto mdata       = std::make_unique<workcell::MotionDeviceData>();
        mdata->kind      = kind;
        data             = std::move(mdata);
        dev              = std::make_unique<workcell::MotionDevice>();
    }

    data->metadata  = std::move(meta);
    data->materials = std::move(materials);
    data->links     = std::move(links);
    data->joints    = std::move(joints);

    try {
        dev->init(std::move(data));
    }
    catch (const std::exception& e) {
        throw std::runtime_error(std::string("DeviceIO::parseDeviceInternal, failed to initialize the device: ") + e.what());
    }

    // Restore the default IK solver when one is recorded in the metadata.
    if (const auto motion = dynamic_cast<workcell::MotionDevice*>(dev.get())) {
        if (motion->kinematics() && motion->metadata().ik_solver_type != kinematics::IKSolverType::None) {
            motion->kinematics()->setIKSolverType(motion->metadata().ik_solver_type);
        }
    }
    dev->setName(dev->metadata().name);
    return dev;
}

std::unique_ptr<tinyxml2::XMLDocument> DeviceIO::buildDoc(const workcell::Device& dev, ExportContext& ctx)
{
    auto           doc = std::make_unique<tinyxml2::XMLDocument>();
    auto* const    xe_device = doc->NewElement("device");
    const auto&    meta = dev.metadata();
    xe_device->SetAttribute("name", toCStr(!meta.name.empty() ? meta.name : meta.model));
    xe_device->SetAttribute("kind", kindToStr(dev.deviceKind()));
    exportVersion(ctx, V_ROBOTICS_IO_VERSION_MAJOR, V_ROBOTICS_IO_VERSION_MINOR, xe_device);
    doc->InsertEndChild(xe_device);
    exportDeviceInternal(ctx, dev, xe_device);
    return doc;
}

std::unique_ptr<workcell::Device> DeviceIO::loadPkg(const std::filesystem::path& pkg_path, const LoadOptions& options)
{
    // Only the archive index is read up front; entries decompress on demand.
    auto pkg = vine::io::ZipArchive::open(pkg_path, vine::io::ZipArchive::OpenMode::ReadOnly);
    if (!pkg) {
        throw std::runtime_error("DeviceIO::loadPkg, not a valid device package: " + pkg_path.string());
    }
    auto dev = loadPkg(*pkg, options);
    dev->setFilePath(pkg_path);
    return dev;
}

std::unique_ptr<workcell::Device> DeviceIO::loadPkg(vine::io::Vfs& vfs, const LoadOptions& options)
{
    return loadXmlFromVfs(vfs, vine::String(u8"device.xml"), options);
}

void DeviceIO::savePkg(const workcell::Device& dev, const std::filesystem::path& pkg_path, const SaveOptions& options)
{
    vine::io::ZipArchive vfs;
    savePkg(dev, vfs, options);
    if (vfs.saveAs(pkg_path) != vine::io::IoError::Ok) {
        throw std::runtime_error("DeviceIO::savePkg, failed to write package file: " + pkg_path.string());
    }
}

void DeviceIO::savePkg(const workcell::Device& dev, vine::io::Vfs& vfs, const SaveOptions& options)
{
    (void)options;
    ExportOptions opts;
    ExportContext ctx(opts);
    ctx.vfs = &vfs;
    const auto           doc = buildDoc(dev, ctx);
    tinyxml2::XMLPrinter printer;
    doc->Print(&printer);
    const String xml(reinterpret_cast<const char8_t*>(printer.CStr()), printer.CStrSize() - 1);
    if (detail::writeText(vfs, vine::String(u8"device.xml"), xml) != vine::io::IoError::Ok) {
        throw std::runtime_error("DeviceIO::savePkg, failed to write device.xml into the vfs.");
    }
}

void DeviceIO::exportDeviceInternal(ExportContext& ctx, const workcell::Device& dev, tinyxml2::XMLElement* xe_device)
{
    exportDeviceMetadata(ctx, dev.metadata(), xe_device);

    // Named material library; visuals reference these by name.
    if (const auto* const data = dev.data()) {
        if (!data->materials.empty()) {
            auto xe_materials = xe_device->GetDocument()->NewElement("materials");
            for (const auto& entry : data->materials) {
                const auto* const color = dynamic_cast<const vine::geometry::ColorMaterial*>(entry.material.get());
                if (color == nullptr) {
                    detail::appendWarning(ctx.msgs, "DeviceIO::exportDeviceInternal, unsupported material type in library, skipped.");
                    continue;
                }
                const auto& c     = color->color();
                std::string rgba  = std::string(detail::doubleToStr(c.r).as_std_str()) + " "
                                  + std::string(detail::doubleToStr(c.g).as_std_str()) + " "
                                  + std::string(detail::doubleToStr(c.b).as_std_str()) + " "
                                  + std::string(detail::doubleToStr(c.a).as_std_str());
                auto xe_mat = xe_device->GetDocument()->NewElement("material");
                xe_mat->SetAttribute("name", toCStr(entry.name));
                xe_mat->SetAttribute("color", rgba.c_str());
                xe_materials->LinkEndChild(xe_mat);
            }
            xe_device->LinkEndChild(xe_materials);
        }
    }

    for (const auto* const link : dev.links()) {
        exportLink(ctx, *link, xe_device);
    }
    for (const auto* const joint : dev.joints()) {
        exportJoint(ctx, *joint, xe_device);
    }

    const auto* const scanner = dynamic_cast<const workcell::Scanner*>(&dev);
    if (scanner == nullptr) {
        return;
    }
    for (const auto* const cam : scanner->cameras()) {
        auto xe_cam = xe_device->GetDocument()->NewElement("camera");
        if (cam) {
            xe_cam->SetAttribute("frame", toCStr(cam->frame ? cam->frame->name() : cam->frame_name));
            for (const char* tag : { "design", "calibrated" }) {
                const auto& intr = std::string(tag) == "design" ? cam->design_intrinsics : cam->calibrated_intrinsics;
                auto        xe_intr = xe_device->GetDocument()->NewElement(tag);
                setAttrDouble(xe_intr, "width", intr.width);
                setAttrDouble(xe_intr, "height", intr.height);
                setAttrDouble(xe_intr, "center_x", intr.center_x);
                setAttrDouble(xe_intr, "center_y", intr.center_y);
                setAttrDouble(xe_intr, "focus_x", intr.focus_x);
                setAttrDouble(xe_intr, "focus_y", intr.focus_y);
                setAttrDouble(xe_intr, "near", intr.near_);
                setAttrDouble(xe_intr, "far", intr.far_);
                xe_cam->LinkEndChild(xe_intr);
            }
        }
        xe_device->LinkEndChild(xe_cam);
    }
    for (const auto* const proj : scanner->projectors()) {
        auto xe_proj = xe_device->GetDocument()->NewElement("projector");
        if (proj) {
            xe_proj->SetAttribute("frame", toCStr(proj->frame ? proj->frame->name() : proj->frame_name));
            for (const char* tag : { "design", "calibrated" }) {
                const auto& params = std::string(tag) == "design" ? proj->design_params : proj->calibrated_params;
                auto        xe_params = xe_device->GetDocument()->NewElement(tag);
                setAttrDouble(xe_params, "fov_w", params.fov_w);
                setAttrDouble(xe_params, "fov_h", params.fov_h);
                setAttrDouble(xe_params, "min_dof", params.min_dof);
                setAttrDouble(xe_params, "max_dof", params.max_dof);
                setAttrDouble(xe_params, "measure_dist", params.measure_dist);
                setAttrDouble(xe_params, "crop_value", params.crop_value);
                xe_proj->LinkEndChild(xe_params);
            }
        }
        xe_device->LinkEndChild(xe_proj);
    }
}

V_ROBOTICS_IO_NS_END
