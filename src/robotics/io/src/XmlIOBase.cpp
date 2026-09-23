#include <vine/robotics/io/XmlIOBase.hpp>

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
#include <vine/io/Vfs.hpp>

#include "IoUtils.hpp"

V_ROBOTICS_IO_NS_BEGIN

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
    return s ? String(reinterpret_cast<const char8_t*>(s)) : String();
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
 * @brief Writes mesh arrays as geoms bin entries into a VFS.
 *
 * @param vfs The active VFS.
 * @param geom_seq Sequence counter for unique bin names (incremented).
 * @param positions Vertex positions.
 * @param normals Optional per-vertex normals.
 * @param texcoords Optional per-vertex texcoords.
 * @param indices Optional triangle indices (empty for a non-indexed mesh).
 * @return The geoms path prefix ("geoms/meshN") used by the XML description.
 */
String writeMeshBins(vine::io::Vfs& vfs, std::size_t& geom_seq,
                     std::span<const vine::math::Vec3f> positions,
                     std::span<const vine::math::Vec3f> normals,
                     std::span<const vine::math::Vec2f> texcoords,
                     std::span<const std::uint32_t> indices)
{
    const std::string seq_str = std::to_string(geom_seq++);
    const String      prefix  = String(u8"geoms/mesh")
                             + String(reinterpret_cast<const char8_t*>(seq_str.data()), seq_str.size());

    const auto write_bin = [&vfs, &prefix](const char8_t* suffix, const std::vector<unsigned char>& bytes) {
        if (vfs.write(prefix + String(suffix), bytes) != vine::io::IoError::Ok) {
            throw std::runtime_error("XmlIOBase::writeMeshBins, failed to write mesh geometry into the package: "
                                     + prefix.as_std_str());
        }
    };

    std::vector<unsigned char> bin;
    detail::vec3ArrayToBytes(positions, bin);
    write_bin(u8".positions.bin", bin);
    if (!normals.empty()) {
        detail::vec3ArrayToBytes(normals, bin);
        write_bin(u8".normals.bin", bin);
    }
    if (!texcoords.empty()) {
        detail::vec2ArrayToBytes(texcoords, bin);
        write_bin(u8".texcoords.bin", bin);
    }
    if (!indices.empty()) {
        detail::uint32ArrayToBytes(indices, bin);
        write_bin(u8".indices.bin", bin);
    }
    return prefix;
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

vine::intrusive_ptr<vine::geometry::Material> XmlIOBase::parseMaterial(ParseContext& ctx,
                                                                       const tinyxml2::XMLElement* xe)
{
    (void)ctx;
    const String color_str = attr(xe, "color");
    if (!color_str.empty()) {
        double r = 0.0, g = 0.0, b = 0.0, a = 1.0;
        std::istringstream stream(color_str.as_std_str());
        if (stream >> r >> g >> b >> a) {
            return vine::make_intrusive<vine::geometry::ColorMaterial>(
                vine::Colorf(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), static_cast<float>(a)));
        }
    }
    return {};
}


vine::intrusive_ptr<vine::geometry::Shape> XmlIOBase::parseGeometry(ParseContext& ctx, const tinyxml2::XMLElement* xe)
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
        return vine::make_intrusive<vine::geometry::Box>(w * s, h * s, d * s);
    }
    if (tag == "sphere") {
        double r = 0.0;
        if (!getAttrDouble(child, "radius", r)) {
            return {};
        }
        return vine::make_intrusive<vine::geometry::Sphere>(r * s);
    }
    if (tag == "cylinder" || tag == "cone") {
        double r = 0.0, h = 0.0;
        if (!getAttrDouble(child, "radius", r) || !getAttrDouble(child, "height", h)) {
            return {};
        }
        if (tag == "cylinder") {
            return vine::make_intrusive<vine::geometry::Cylinder>(r * s, h * s);
        }
        return vine::make_intrusive<vine::geometry::Cone>(r * s, h * s);
    }
    if (tag == "ellipsoid") {
        const String radii_str = attr(child, "radii");
        math::Vec3d  radii;
        if (radii_str.empty() || !detail::strToVec3(radii_str, radii)) {
            return {};
        }
        return vine::intrusive_ptr<vine::geometry::Ellipsoid>(
            new vine::geometry::Ellipsoid(radii.x * s, radii.y * s, radii.z * s));
    }
    if (tag == "triangle_mesh" || tag == "indexed_triangle_mesh") {
        if (ctx.vfs == nullptr) {
            detail::appendWarning(ctx.msgs, "XmlIOBase::parseGeometry, mesh requires a package VFS, skipped.");
            return {};
        }
        const String pos_path = attr(child, "positions");
        if (pos_path.empty()) {
            return {};
        }
        const auto bytes = ctx.vfs->read(pos_path);
        if (!bytes) {
            return {};
        }
        vine::geometry::Vec3fArray positions;
        if (!detail::bytesToVec3Array(bytes.value(), positions)) {
            return {};
        }
        vine::geometry::Vec3fArray normals;
        const String               nrm_path = attr(child, "normals");
        if (!nrm_path.empty()) {
            const auto nrm_bytes = ctx.vfs->read(nrm_path);
            if (!nrm_bytes || !detail::bytesToVec3Array(nrm_bytes.value(), normals)) {
                return {};
            }
        }
        vine::geometry::Vec2fArray texcoords;
        const String               uv_path = attr(child, "texcoords");
        if (!uv_path.empty()) {
            const auto uv_bytes = ctx.vfs->read(uv_path);
            if (!uv_bytes || !detail::bytesToVec2Array(uv_bytes.value(), texcoords)) {
                return {};
            }
        }
        if (tag == "triangle_mesh") {
            auto mesh = vine::make_intrusive<vine::geometry::TriangleMesh>();
            mesh->setPositions(std::move(positions));
            if (!normals.empty()) {
                mesh->setNormals(std::move(normals));
            }
            if (!texcoords.empty()) {
                mesh->setTexcoords(std::move(texcoords));
            }
            return mesh;
        }
        const String idx_path = attr(child, "indices");
        if (idx_path.empty()) {
            return {};
        }
        const auto idx_bytes = ctx.vfs->read(idx_path);
        if (!idx_bytes) {
            return {};
        }
        vine::geometry::UInt32Array indices;
        if (!detail::bytesToUInt32Array(idx_bytes.value(), indices)) {
            return {};
        }
        auto mesh = vine::intrusive_ptr<vine::geometry::IndexedTriangleMesh>(
            new vine::geometry::IndexedTriangleMesh());
        mesh->setPositions(std::move(positions));
        if (!normals.empty()) {
            mesh->setNormals(std::move(normals));
        }
        if (!texcoords.empty()) {
            mesh->setTexcoords(std::move(texcoords));
        }
        mesh->setIndices(std::move(indices));
        return mesh;
    }
    detail::appendWarning(ctx.msgs, "XmlIOBase::parseGeometry, unsupported geometry tag [%s], skipped.", tag.c_str());
    return {};
}

void XmlIOBase::exportGeometry(ExportContext& ctx, const vine::geometry::Shape& shape, tinyxml2::XMLElement* xe)
{
    (void)ctx;
    auto xe_geom = xe->GetDocument()->NewElement("geometry");
    switch (shape.shapeType()) {
    case vine::geometry::ShapeType::Box: {
        const auto* const box = dynamic_cast<const vine::geometry::Box*>(&shape);
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
    case vine::geometry::ShapeType::Cylinder: {
        const auto* const cyli = dynamic_cast<const vine::geometry::Cylinder*>(&shape);
        if (cyli == nullptr) {
            return;
        }
        auto xe_cyli = xe->GetDocument()->NewElement("cylinder");
        setAttrDouble(xe_cyli, "radius", cyli->radius());
        setAttrDouble(xe_cyli, "height", cyli->height());
        xe_geom->LinkEndChild(xe_cyli);
        break;
    }
    case vine::geometry::ShapeType::Cone: {
        const auto* const cone = dynamic_cast<const vine::geometry::Cone*>(&shape);
        if (cone == nullptr) {
            return;
        }
        auto xe_cone = xe->GetDocument()->NewElement("cone");
        setAttrDouble(xe_cone, "radius", cone->radius());
        setAttrDouble(xe_cone, "height", cone->height());
        xe_geom->LinkEndChild(xe_cone);
        break;
    }
    case vine::geometry::ShapeType::Sphere: {
        const auto* const sphere = dynamic_cast<const vine::geometry::Sphere*>(&shape);
        if (sphere == nullptr) {
            return;
        }
        auto xe_sphere = xe->GetDocument()->NewElement("sphere");
        setAttrDouble(xe_sphere, "radius", sphere->radius());
        xe_geom->LinkEndChild(xe_sphere);
        break;
    }
    case vine::geometry::ShapeType::Ellipsoid: {
        const auto* const ellipsoid = dynamic_cast<const vine::geometry::Ellipsoid*>(&shape);
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
    case vine::geometry::ShapeType::TriangleMesh: {
        const auto* const mesh = dynamic_cast<const vine::geometry::TriangleMesh*>(&shape);
        if (mesh == nullptr) {
            return;
        }
        if (ctx.vfs == nullptr) {
            detail::appendWarning(ctx.msgs, "XmlIOBase::exportGeometry, triangle mesh requires a package VFS, skipped.");
            return;
        }
        String prefix;
        const auto it = ctx.mesh_paths.find(&shape);
        if (it != ctx.mesh_paths.end()) {
            prefix = it->second;
        }
        else {
            prefix = writeMeshBins(*ctx.vfs, ctx.geom_seq, mesh->positions(), mesh->normals(),
                                   mesh->texcoords(), {});
            ctx.mesh_paths[&shape] = prefix;
        }
        auto xe_mesh = xe->GetDocument()->NewElement("triangle_mesh");
        const auto vertex_count_str = std::to_string(mesh->vertexCount());
        const auto triangle_count_str = std::to_string(mesh->triangleCount());
        xe_mesh->SetAttribute("vertex_count", vertex_count_str.c_str());
        xe_mesh->SetAttribute("triangle_count", triangle_count_str.c_str());
        setAttr(xe_mesh, "positions", prefix + String(u8".positions.bin"));
        if (!mesh->normals().empty()) {
            setAttr(xe_mesh, "normals", prefix + String(u8".normals.bin"));
        }
        if (!mesh->texcoords().empty()) {
            setAttr(xe_mesh, "texcoords", prefix + String(u8".texcoords.bin"));
        }
        xe_geom->LinkEndChild(xe_mesh);
        break;
    }
    case vine::geometry::ShapeType::IndexedTriangleMesh: {
        const auto* const mesh = dynamic_cast<const vine::geometry::IndexedTriangleMesh*>(&shape);
        if (mesh == nullptr) {
            return;
        }
        if (ctx.vfs == nullptr) {
            detail::appendWarning(ctx.msgs, "XmlIOBase::exportGeometry, indexed triangle mesh requires a package VFS, skipped.");
            return;
        }
        String prefix;
        const auto it = ctx.mesh_paths.find(&shape);
        if (it != ctx.mesh_paths.end()) {
            prefix = it->second;
        }
        else {
            prefix = writeMeshBins(*ctx.vfs, ctx.geom_seq, mesh->positions(), mesh->normals(),
                                   mesh->texcoords(), mesh->indices());
            ctx.mesh_paths[&shape] = prefix;
        }
        auto xe_mesh = xe->GetDocument()->NewElement("indexed_triangle_mesh");
        const auto vertex_count_str = std::to_string(mesh->vertexCount());
        const auto triangle_count_str = std::to_string(mesh->triangleCount());
        xe_mesh->SetAttribute("vertex_count", vertex_count_str.c_str());
        xe_mesh->SetAttribute("triangle_count", triangle_count_str.c_str());
        setAttr(xe_mesh, "positions", prefix + String(u8".positions.bin"));
        if (!mesh->normals().empty()) {
            setAttr(xe_mesh, "normals", prefix + String(u8".normals.bin"));
        }
        if (!mesh->texcoords().empty()) {
            setAttr(xe_mesh, "texcoords", prefix + String(u8".texcoords.bin"));
        }
        setAttr(xe_mesh, "indices", prefix + String(u8".indices.bin"));
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

V_ROBOTICS_IO_NS_END
