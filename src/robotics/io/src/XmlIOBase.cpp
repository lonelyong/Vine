#include <vine/robotics/io/XmlIOBase.hpp>

#include <cstring>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <tinyxml2.h>

#include <vine/geometry/Box.hpp>
#include <vine/geometry/Cone.hpp>
#include <vine/geometry/ColorMaterial.hpp>
#include <vine/geometry/Cylinder.hpp>
#include <vine/geometry/Ellipsoid.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/Sphere.hpp>
#include <vine/geometry/TriangleMesh.hpp>
#include <vine/io/Adapters.hpp>
#include <vine/io/Vfs.hpp>

#include "IoUtils.hpp"
#include "MeshBin.hpp"

VN_ROBOTICS_IO_NS_BEGIN

namespace
{

/**
 * @brief Converts a String to a C string for tinyxml2.
 *
 * @param s The string.
 * @return The UTF-8 byte pointer.
 */
const char* toCStr(const String& s)
{
    return reinterpret_cast<const char*>(s.c_str());
}

/**
 * @brief Converts a C string to a String.
 *
 * @param s The UTF-8 byte pointer, or null.
 * @return The string, empty when null.
 */
String fromCStr(const char* s)
{
    return s ? String::fromUtf8(s) : String{};
}

/**
 * @brief Reads a double attribute.
 *
 * @param xe The element.
 * @param name The attribute name.
 * @param out Receives the value.
 * @return true when the attribute exists and parsed.
 */
bool getAttrDouble(const tinyxml2::XMLElement* xe, const char* name, double& out)
{
    const char* const value = xe->Attribute(name);
    return value != nullptr && detail::strToDouble(fromCStr(value), out);
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
    xe->SetAttribute(name, toCStr(detail::doubleToStr(value)));
}

/**
 * @brief Returns a child element's text or attribute value.
 *
 * @param xe The element.
 * @param name The attribute name.
 * @return The value, or empty.
 */
String attr(const tinyxml2::XMLElement* xe, const char* name)
{
    return fromCStr(xe->Attribute(name));
}

/**
 * @brief Writes a string attribute.
 *
 * @param xe The element.
 * @param name The attribute name.
 * @param value The value.
 */
void setAttr(tinyxml2::XMLElement* xe, const char* name, const String& value)
{
    xe->SetAttribute(name, toCStr(value));
}

/**
 * @brief Writes a mesh as one geometry file into a VFS.
 *
 * A mesh is one file, never one entry per array. Which form that file takes is decided by what it has to hold: binary STL
 * when STL holds all of it, and the self-describing form when the mesh carries texture coordinates STL has no place for
 * (see MeshBin.hpp). The arrays are read straight from the buffers that already hold them, so the geometry is not copied
 * on its way into the package - and the source holds them, so a backend that writes the package later needs nothing kept
 * alive by the caller.
 *
 * @param vfs The active VFS.
 * @param geom_seq Sequence counter for unique names (incremented).
 * @param mesh The mesh to write.
 * @return The virtual path of the file ("geoms/meshN.stl" or "geoms/meshN.vmesh"), which the description points at.
 */
String writeMeshFile(vn::io::Vfs& vfs, std::size_t& geom_seq, const vn::geometry::Mesh& mesh)
{
    const std::string seq_str = std::to_string(geom_seq++);
    const String      prefix  = String(u8"geoms/mesh") + String::fromUtf8(seq_str);

    String                              path;
    std::shared_ptr<vn::io::DataSource> source;
    if (detail::stlWouldLose(mesh)) {
        path   = prefix + String(u8".vmesh");
        source = std::make_shared<detail::MeshBinSource>(mesh);
    }
    else {
        path   = prefix + String(u8".stl");
        source = std::make_shared<detail::StlMeshSource>(mesh);
    }

    if (vfs.addFile(detail::vfsPath(path), std::move(source)) != vn::io::IoError::Ok) {
        throw std::runtime_error("XmlIOBase::exportGeometry, failed to write mesh geometry into the package: "
                                 + path.as_std_str());
    }
    return path;
}

} // namespace

XmlIOBase::~XmlIOBase() = default;

void XmlIOBase::parseVersion(ParseContext& ctx, uint16_t& major, uint16_t& minor, const tinyxml2::XMLElement* xe)
{
    (void)ctx;
    const String version = attr(xe, "version");
    major               = 1;
    minor               = 0;
    if (version.empty()) {
        return;
    }
    std::size_t dot = 0;
    while (dot < version.size() && version[dot] != u8'.') {
        ++dot;
    }
    String major_str = String(version.as_std_u8str().substr(0, dot));
    String minor_str = dot + 1 < version.size() ? String(version.as_std_u8str().substr(dot + 1)) : String();
    if (major_str.empty()) {
        throw std::runtime_error("XmlIOBase::parseVersion, invalid version string.");
    }
    try {
        major = static_cast<uint16_t>(std::stoul(major_str.as_std_str()));
        minor = minor_str.empty() ? 0 : static_cast<uint16_t>(std::stoul(minor_str.as_std_str()));
    }
    catch (const std::exception&) {
        throw std::runtime_error("XmlIOBase::parseVersion, invalid version string.");
    }
}

void XmlIOBase::exportVersion(ExportContext& ctx, uint16_t major, uint16_t minor, tinyxml2::XMLElement* xe)
{
    (void)ctx;
    std::string version = std::to_string(major) + "." + std::to_string(minor);
    xe->SetAttribute("version", version.c_str());
}

void XmlIOBase::parsePose(ParseContext& ctx, math::Isometry3d& pose, const tinyxml2::XMLElement* xe)
{
    pose = math::Isometry3d{};
    const String xyz_str = attr(xe, "xyz");
    if (!xyz_str.empty()) {
        math::Vec3d xyz;
        if (detail::strToVec3(xyz_str, xyz)) {
            pose.translation = math::Point3d(xyz.x * ctx.options.len_unit_scaling,
                                             xyz.y * ctx.options.len_unit_scaling,
                                             xyz.z * ctx.options.len_unit_scaling);
        }
    }
    const String quat_str = attr(xe, "quat");
    if (!quat_str.empty()) {
        double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
        std::istringstream stream(quat_str.as_std_str());
        if (stream >> x >> y >> z >> w) {
            pose.rotation = math::Quatd(x, y, z, w);
        }
    }
}

void XmlIOBase::exportPose(ExportContext& ctx, const math::Isometry3d& pose, tinyxml2::XMLElement* xe)
{
    (void)ctx;
    const auto  origin   = xe->GetDocument()->NewElement("origin");
    const auto& t        = pose.translation;
    const auto& q        = pose.rotation;
    std::string xyz      = std::string(detail::doubleToStr(t.x).as_std_str()) + " " + std::string(detail::doubleToStr(t.y).as_std_str())
                     + " " + std::string(detail::doubleToStr(t.z).as_std_str());
    std::string quat     = std::string(detail::doubleToStr(q.x).as_std_str()) + " " + std::string(detail::doubleToStr(q.y).as_std_str())
                     + " " + std::string(detail::doubleToStr(q.z).as_std_str()) + " " + std::string(detail::doubleToStr(q.w).as_std_str());
    origin->SetAttribute("xyz", xyz.c_str());
    origin->SetAttribute("quat", quat.c_str());
    xe->LinkEndChild(origin);
}

vn::intrusive_ptr<vn::geometry::Material> XmlIOBase::parseMaterial(ParseContext& ctx,
                                                                       const tinyxml2::XMLElement* xe)
{
    (void)ctx;
    const String color_str = attr(xe, "color");
    if (!color_str.empty()) {
        double r = 0.0, g = 0.0, b = 0.0, a = 1.0;
        std::istringstream stream(color_str.as_std_str());
        if (stream >> r >> g >> b >> a) {
            return vn::make_intrusive<vn::geometry::ColorMaterial>(
                vn::Colorf(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), static_cast<float>(a)));
        }
    }
    return {};
}

vn::intrusive_ptr<vn::geometry::Shape> XmlIOBase::parseGeometry(ParseContext& ctx, const tinyxml2::XMLElement* xe)
{
    const tinyxml2::XMLElement* const child = xe->FirstChildElement();
    if (child == nullptr) {
        return {};
    }
    const std::string tag = child->Name();
    const double      s   = ctx.options.len_unit_scaling;
    if (tag == "box") {
        double w = 0.0, h = 0.0, d = 0.0;
        // size is space-separated "w h d"
        const String        size_str = attr(child, "size");
        std::istringstream  stream(size_str.as_std_str());
        if (!(stream >> w >> h >> d)) {
            return {};
        }
        return vn::make_intrusive<vn::geometry::Box>(w * s, h * s, d * s);
    }
    if (tag == "sphere") {
        double r = 0.0;
        if (!getAttrDouble(child, "radius", r)) {
            return {};
        }
        return vn::make_intrusive<vn::geometry::Sphere>(r * s);
    }
    if (tag == "cylinder" || tag == "cone") {
        double r = 0.0, h = 0.0;
        if (!getAttrDouble(child, "radius", r) || !getAttrDouble(child, "height", h)) {
            return {};
        }
        if (tag == "cylinder") {
            return vn::make_intrusive<vn::geometry::Cylinder>(r * s, h * s);
        }
        return vn::make_intrusive<vn::geometry::Cone>(r * s, h * s);
    }
    if (tag == "ellipsoid") {
        const String radii_str = attr(child, "radii");
        math::Vec3d  radii;
        if (radii_str.empty() || !detail::strToVec3(radii_str, radii)) {
            return {};
        }
        return vn::intrusive_ptr<vn::geometry::Ellipsoid>(
            new vn::geometry::Ellipsoid(radii.x * s, radii.y * s, radii.z * s));
    }
    if (tag == "triangle_mesh" || tag == "indexed_triangle_mesh") {
        if (ctx.vfs == nullptr) {
            detail::appendWarning(ctx.msgs, "XmlIOBase::parseGeometry, mesh requires a package VFS, skipped.");
            return {};
        }
        const bool indexed = tag == "indexed_triangle_mesh";

        // The description points at one file per mesh. That file is a binary STL or a .vmesh, and each reader checks
        // that the bytes are what the path says they are, so a description and its file cannot drift apart silently.
        const String geometry_path = attr(child, "geometry");
        if (geometry_path.empty()) {
            return {}; // a mesh element that names no file has nothing to read
        }
        return detail::meshFromFile(*ctx.vfs, detail::vfsPath(geometry_path), indexed);
    }
    detail::appendWarning(ctx.msgs, "XmlIOBase::parseGeometry, unsupported geometry tag [%s], skipped.", tag.c_str());
    return {};
}

void XmlIOBase::exportGeometry(ExportContext& ctx, const vn::geometry::Shape& shape, tinyxml2::XMLElement* xe)
{
    (void)ctx;
    auto xe_geom = xe->GetDocument()->NewElement("geometry");
    switch (shape.shapeType()) {
    case vn::geometry::ShapeType::Box: {
        const auto* const box = dynamic_cast<const vn::geometry::Box*>(&shape);
        if (box == nullptr) {
            return;
        }
        auto        xe_box = xe->GetDocument()->NewElement("box");
        std::string size   = std::string(detail::doubleToStr(box->width()).as_std_str()) + " "
                         + std::string(detail::doubleToStr(box->height()).as_std_str()) + " "
                         + std::string(detail::doubleToStr(box->depth()).as_std_str());
        xe_box->SetAttribute("size", size.c_str());
        xe_geom->LinkEndChild(xe_box);
        break;
    }
    case vn::geometry::ShapeType::Cylinder: {
        const auto* const cyli = dynamic_cast<const vn::geometry::Cylinder*>(&shape);
        if (cyli == nullptr) {
            return;
        }
        auto xe_cyli = xe->GetDocument()->NewElement("cylinder");
        setAttrDouble(xe_cyli, "radius", cyli->radius());
        setAttrDouble(xe_cyli, "height", cyli->height());
        xe_geom->LinkEndChild(xe_cyli);
        break;
    }
    case vn::geometry::ShapeType::Cone: {
        const auto* const cone = dynamic_cast<const vn::geometry::Cone*>(&shape);
        if (cone == nullptr) {
            return;
        }
        auto xe_cone = xe->GetDocument()->NewElement("cone");
        setAttrDouble(xe_cone, "radius", cone->radius());
        setAttrDouble(xe_cone, "height", cone->height());
        xe_geom->LinkEndChild(xe_cone);
        break;
    }
    case vn::geometry::ShapeType::Sphere: {
        const auto* const sphere = dynamic_cast<const vn::geometry::Sphere*>(&shape);
        if (sphere == nullptr) {
            return;
        }
        auto xe_sphere = xe->GetDocument()->NewElement("sphere");
        setAttrDouble(xe_sphere, "radius", sphere->radius());
        xe_geom->LinkEndChild(xe_sphere);
        break;
    }
    case vn::geometry::ShapeType::Ellipsoid: {
        const auto* const ellipsoid = dynamic_cast<const vn::geometry::Ellipsoid*>(&shape);
        if (ellipsoid == nullptr) {
            return;
        }
        auto        xe_ell = xe->GetDocument()->NewElement("ellipsoid");
        std::string radii  = std::string(detail::doubleToStr(ellipsoid->radiusX()).as_std_str()) + " "
                          + std::string(detail::doubleToStr(ellipsoid->radiusY()).as_std_str()) + " "
                          + std::string(detail::doubleToStr(ellipsoid->radiusZ()).as_std_str());
        xe_ell->SetAttribute("radii", radii.c_str());
        xe_geom->LinkEndChild(xe_ell);
        break;
    }
    case vn::geometry::ShapeType::TriangleMesh:
    case vn::geometry::ShapeType::IndexedTriangleMesh: {
        const auto* const mesh = dynamic_cast<const vn::geometry::Mesh*>(&shape);
        if (mesh == nullptr) {
            return;
        }
        if (ctx.vfs == nullptr) {
            detail::appendWarning(ctx.msgs, "XmlIOBase::exportGeometry, triangle mesh requires a package VFS, skipped.");
            return;
        }

        String     path;
        const auto it = ctx.mesh_paths.find(&shape);
        if (it != ctx.mesh_paths.end()) {
            path = it->second; // a mesh several visuals or collisions share is written once
        }
        else {
            path                   = writeMeshFile(*ctx.vfs, ctx.geom_seq, *mesh);
            ctx.mesh_paths[&shape] = path;
        }

        // The element describes the FILE, not the shape in memory: a binary STL file is a soup whatever the mesh is, and
        // it holds no per-vertex arrays, so only the triangle count is worth stating.
        const detail::MeshArrays counts       = detail::MeshArrays::take(*mesh);
        const bool               soup_file    = detail::isStlPath(detail::vfsPath(path));
        const bool               indexed_file = !soup_file && mesh->shapeType() == vn::geometry::ShapeType::IndexedTriangleMesh;
        auto xe_mesh = xe->GetDocument()->NewElement(indexed_file ? "indexed_triangle_mesh" : "triangle_mesh");
        const auto triangle_count_str = std::to_string(counts.triangle_count);
        xe_mesh->SetAttribute("triangle_count", triangle_count_str.c_str());
        if (!soup_file) {
            const auto vertex_count_str = std::to_string(counts.vertex_count);
            xe_mesh->SetAttribute("vertex_count", vertex_count_str.c_str());
        }
        setAttr(xe_mesh, "geometry", path);
        xe_geom->LinkEndChild(xe_mesh);
        break;
    }
    default:
        detail::appendWarning(ctx.msgs, "XmlIOBase::exportGeometry, unsupported shape type, skipped.");
        return;
    }
    xe->LinkEndChild(xe_geom);
}

void XmlIOBase::parseVisual(ParseContext& ctx, workcell::Visual& visual, const tinyxml2::XMLElement* xe)
{
    visual.setTf(math::Isometry3d{});
    if (const auto xe_origin = xe->FirstChildElement("origin")) {
        math::Isometry3d tf;
        parsePose(ctx, tf, xe_origin);
        visual.setTf(tf);
    }
    if (const auto xe_geom = xe->FirstChildElement("geometry")) {
        visual.setShape(parseGeometry(ctx, xe_geom));
    }
    if (const auto xe_mat = xe->FirstChildElement("material")) {
        const String mat_name = attr(xe_mat, "name");
        if (mat_name.empty()) {
            // Materials must be defined in the device <materials> library
            // first and referenced by name; in-place materials are not used.
            detail::appendWarning(ctx.msgs, "XmlIOBase::parseVisual, in-place material is not supported; use a device material name.");
        }
        else {
            const auto it = ctx.materials_by_name.find(mat_name);
            if (it != ctx.materials_by_name.end()) {
                visual.setMaterial(it->second);
                visual.setMaterialName(mat_name);
            }
            else {
                detail::appendWarning(ctx.msgs, "XmlIOBase::parseVisual, material [%s] not found in the device.", toCStr(mat_name));
            }
        }
    }
}

void XmlIOBase::exportVisual(ExportContext& ctx, const workcell::Visual& visual, tinyxml2::XMLElement* xe)
{
    auto xe_visual = xe->GetDocument()->NewElement("visual");
    exportPose(ctx, visual.tf(), xe_visual);
    if (visual.shape()) {
        exportGeometry(ctx, *visual.shape(), xe_visual);
    }
    if (!visual.materialName().empty()) {
        // Reference to a device-level material by name.
        auto xe_mat = xe_visual->GetDocument()->NewElement("material");
        xe_mat->SetAttribute("name", toCStr(visual.materialName()));
        xe_visual->LinkEndChild(xe_mat);
    }
    else if (visual.material()) {
        // In-place materials are not used; materials must be defined on the
        // device and referenced by name.
        detail::appendWarning(ctx.msgs, "XmlIOBase::exportVisual, in-place material is not supported; "
                "materials must be defined on the device and referenced by name.");
    }
    xe->LinkEndChild(xe_visual);
}

void XmlIOBase::parseCollision(ParseContext& ctx, workcell::Collision& collision, const tinyxml2::XMLElement* xe)
{
    collision.setTf(math::Isometry3d{});
    if (const auto xe_origin = xe->FirstChildElement("origin")) {
        math::Isometry3d tf;
        parsePose(ctx, tf, xe_origin);
        collision.setTf(tf);
    }
    if (const auto xe_geom = xe->FirstChildElement("geometry")) {
        collision.setShape(parseGeometry(ctx, xe_geom));
    }
}

void XmlIOBase::exportCollision(ExportContext& ctx, const workcell::Collision& collision, tinyxml2::XMLElement* xe)
{
    auto xe_collision = xe->GetDocument()->NewElement("collision");
    exportPose(ctx, collision.tf(), xe_collision);
    if (collision.shape()) {
        exportGeometry(ctx, *collision.shape(), xe_collision);
    }
    xe->LinkEndChild(xe_collision);
}

void XmlIOBase::parseLink(ParseContext& ctx, workcell::Link& link, const tinyxml2::XMLElement* xe)
{
    link.setName(attr(xe, "name"));
    auto& body = link.body();
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

void XmlIOBase::exportLink(ExportContext& ctx, const workcell::Link& link, tinyxml2::XMLElement* xe)
{
    auto xe_link = xe->GetDocument()->NewElement("link");
    xe_link->SetAttribute("name", toCStr(link.name()));
    for (const auto& visual : link.body().visuals()) {
        exportVisual(ctx, visual, xe_link);
    }
    for (const auto& collision : link.body().collisions()) {
        exportCollision(ctx, collision, xe_link);
    }
    xe->LinkEndChild(xe_link);
}

std::unique_ptr<workcell::Joint> XmlIOBase::parseJoint(ParseContext& ctx,
                                                       tinyxml2::XMLElement*                    xe,
                                                       const std::map<String, raw_ptr<workcell::Link>>& link_by_name)
{
    const String name = attr(xe, "name");

    kinematics::FrameType type = kinematics::FrameType::Fixed;
    const String          type_str = attr(xe, "type");
    if (type_str == u8"revolute") {
        type = kinematics::FrameType::RevoluteJoint;
    } else if (type_str == u8"prismatic") {
        type = kinematics::FrameType::PrismaticJoint;
    } else if (type_str == u8"planar") {
        type = kinematics::FrameType::PlanarJoint;
    } else if (!type_str.empty() && type_str != u8"fixed") {
        detail::appendWarning(ctx.msgs, "XmlIOBase::parseJoint, (xml:%d) unknown joint type [%s], treated as fixed.", xe->GetLineNum(),
                toCStr(type_str));
    }

    auto joint = std::make_unique<workcell::Joint>(type);
    joint->setName(name);

    if (const auto xe_origin = xe->FirstChildElement("origin")) {
        math::Isometry3d origin;
        parsePose(ctx, origin, xe_origin);
        joint->setFixedTransform(origin);
    }

    const String parent_name = attr(xe, "parent");
    if (!parent_name.empty()) {
        if (const auto it = link_by_name.find(parent_name); it != link_by_name.end()) {
            joint->setParentLink(it->second);
        } else {
            detail::appendWarning(ctx.msgs, "XmlIOBase::parseJoint, (xml:%d) parent link [%s] not found.", xe->GetLineNum(), toCStr(parent_name));
        }
    }
    const String child_name = attr(xe, "child");
    if (!child_name.empty()) {
        if (const auto it = link_by_name.find(child_name); it != link_by_name.end()) {
            joint->setChildLink(it->second);
        } else {
            detail::appendWarning(ctx.msgs, "XmlIOBase::parseJoint, (xml:%d) child link [%s] not found.", xe->GetLineNum(), toCStr(child_name));
        }
    }

    std::vector<kinematics::DofInfo> dofs;
    for (auto* xe_dof = xe->FirstChildElement("dof"); xe_dof; xe_dof = xe_dof->NextSiblingElement("dof")) {
        kinematics::DofInfo dof;
        const String        dof_type = attr(xe_dof, "type");
        dof.type = dof_type == u8"prismatic" ? kinematics::DofType::PrismaticJoint : kinematics::DofType::RevoluteJoint;
        parsePose(ctx, dof.origin, xe_dof);
        const String axis_str = attr(xe_dof, "axis");
        if (!axis_str.empty()) {
            detail::strToVec3(axis_str, dof.axis);
        }
        getAttrDouble(xe_dof, "lower", dof.lower);
        getAttrDouble(xe_dof, "upper", dof.upper);
        getAttrDouble(xe_dof, "velocity", dof.velocity_limit);
        getAttrDouble(xe_dof, "acceleration", dof.acceleration_limit);
        dofs.push_back(dof);
    }
    joint->setDofInfos(dofs);
    return joint;
}

void XmlIOBase::exportJoint(ExportContext& ctx, const workcell::Joint& joint, tinyxml2::XMLElement* xe)
{
    auto xe_joint = xe->GetDocument()->NewElement("joint");
    xe_joint->SetAttribute("name", toCStr(joint.name()));

    const char* type_str = "fixed";
    switch (joint.frameType()) {
    case kinematics::FrameType::RevoluteJoint:
        type_str = "revolute";
        break;
    case kinematics::FrameType::PrismaticJoint:
        type_str = "prismatic";
        break;
    case kinematics::FrameType::PlanarJoint:
        type_str = "planar";
        break;
    default:
        break;
    }
    xe_joint->SetAttribute("type", type_str);
    if (joint.parentLink()) {
        xe_joint->SetAttribute("parent", toCStr(joint.parentLink()->name()));
    }
    if (joint.childLink()) {
        xe_joint->SetAttribute("child", toCStr(joint.childLink()->name()));
    }
    exportPose(ctx, joint.fixedTransform(), xe_joint);

    for (const auto& dof : joint.dofInfos()) {
        auto xe_dof = xe->GetDocument()->NewElement("dof");
        xe_dof->SetAttribute("type", dof.type == kinematics::DofType::PrismaticJoint ? "prismatic" : "revolute");
        xe_dof->SetAttribute("axis", toCStr(detail::vec3ToStr(dof.axis)));
        // The dof origin is flattened as xyz / quat attributes on the <dof> node.
        const auto& t        = dof.origin.translation;
        const auto& q        = dof.origin.rotation;
        std::string xyz      = std::string(detail::doubleToStr(t.x).as_std_str()) + " "
                         + std::string(detail::doubleToStr(t.y).as_std_str()) + " "
                         + std::string(detail::doubleToStr(t.z).as_std_str());
        std::string quat     = std::string(detail::doubleToStr(q.x).as_std_str()) + " "
                         + std::string(detail::doubleToStr(q.y).as_std_str()) + " "
                         + std::string(detail::doubleToStr(q.z).as_std_str()) + " "
                         + std::string(detail::doubleToStr(q.w).as_std_str());
        xe_dof->SetAttribute("xyz", xyz.c_str());
        xe_dof->SetAttribute("quat", quat.c_str());
        setAttrDouble(xe_dof, "lower", dof.lower);
        setAttrDouble(xe_dof, "upper", dof.upper);
        setAttrDouble(xe_dof, "velocity", dof.velocity_limit);
        setAttrDouble(xe_dof, "acceleration", dof.acceleration_limit);
        xe_joint->LinkEndChild(xe_dof);
    }
    xe->LinkEndChild(xe_joint);
}

void XmlIOBase::parseDeviceMetadata(ParseContext& ctx, workcell::DeviceMetadata& metadata, const tinyxml2::XMLElement* xe)
{
    (void)ctx;
    metadata.id          = attr(xe, "id");
    metadata.name        = attr(xe, "name");
    metadata.description = attr(xe, "description");
    metadata.sn          = attr(xe, "sn");
    metadata.manufacturer = attr(xe, "manufacturer");
    metadata.model       = attr(xe, "model");
    metadata.author      = attr(xe, "author");
    metadata.version     = attr(xe, "version");
    metadata.create_time = attr(xe, "create_time");
    metadata.modified_time = attr(xe, "modified_time");

    const String unit_str = attr(xe, "length_unit");
    metadata.length_unit  = unit_str == u8"m" ? workcell::LengthUnit::Meter : workcell::LengthUnit::Millimeter;

    const String ik_str = attr(xe, "iksolver");
    if (ik_str == u8"None") {
        metadata.ik_solver_type = kinematics::IKSolverType::None;
    } else if (ik_str == u8"Pieper") {
        metadata.ik_solver_type = kinematics::IKSolverType::Pieper;
    } else {
        metadata.ik_solver_type = kinematics::IKSolverType::Iterative;
    }
}

void XmlIOBase::exportDeviceMetadata(ExportContext& ctx, const workcell::DeviceMetadata& metadata,
                                     tinyxml2::XMLElement* xe)
{
    (void)ctx;
    auto xe_meta = xe->GetDocument()->NewElement("metadata");
    if (!metadata.id.empty()) {
        xe_meta->SetAttribute("id", toCStr(metadata.id));
    }
    if (!metadata.name.empty()) {
        xe_meta->SetAttribute("name", toCStr(metadata.name));
    }
    if (!metadata.description.empty()) {
        xe_meta->SetAttribute("description", toCStr(metadata.description));
    }
    if (!metadata.sn.empty()) {
        xe_meta->SetAttribute("sn", toCStr(metadata.sn));
    }
    if (!metadata.manufacturer.empty()) {
        xe_meta->SetAttribute("manufacturer", toCStr(metadata.manufacturer));
    }
    if (!metadata.model.empty()) {
        xe_meta->SetAttribute("model", toCStr(metadata.model));
    }
    if (!metadata.author.empty()) {
        xe_meta->SetAttribute("author", toCStr(metadata.author));
    }
    if (!metadata.version.empty()) {
        xe_meta->SetAttribute("version", toCStr(metadata.version));
    }
    if (!metadata.create_time.empty()) {
        xe_meta->SetAttribute("create_time", toCStr(metadata.create_time));
    }
    if (!metadata.modified_time.empty()) {
        xe_meta->SetAttribute("modified_time", toCStr(metadata.modified_time));
    }
    xe_meta->SetAttribute("length_unit", metadata.length_unit == workcell::LengthUnit::Meter ? "m" : "mm");
    if (metadata.ik_solver_type != kinematics::IKSolverType::None) {
        const char* ik = "Iterative";
        if (metadata.ik_solver_type == kinematics::IKSolverType::Pieper) {
            ik = "Pieper";
        }
        xe_meta->SetAttribute("iksolver", ik);
    }
    xe->LinkEndChild(xe_meta);
}

VN_ROBOTICS_IO_NS_END
