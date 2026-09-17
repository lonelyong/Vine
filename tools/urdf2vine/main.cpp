/**
 * @file main.cpp
 * @brief Converts a URDF device (with STL meshes) into a Vine .vdevpkg.
 *
 * Reads a URDF resource folder, builds an in-memory workcell::MotionDevice
 * (joints/links from URDF, meshes loaded via vine::meshio::MeshLoader,
 * scaled to mm) and reuses DeviceIO::savePkg so the output format is
 * identical to what the RoboticsIO module produces. Loads the package back
 * for verification.
 *
 * Usage:
 *   urdf2vine <device.urdf> [output.vdevpkg]
 */

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <tinyxml2.h>

#include <vine/geometry/ColorMaterial.hpp>
#include <vine/math/Isometry3.hpp>
#include <vine/meshio/MeshLoader.hpp>
#include <vine/robotics/kinematics/DofInfo.hpp>
#include <vine/robotics/kinematics/Frame.hpp>
#include <vine/robotics/kinematics/KinematicsBase.hpp>
#include <vine/robotics/io/DeviceIO.hpp>
#include <vine/robotics/workcell/Device.hpp>
#include <vine/robotics/workcell/MotionDevice.hpp>
#include <vine/String.hpp>

using namespace vine;
using namespace vine::math;
using namespace vine::robotics;
using namespace vine::robotics::kinematics;
using namespace vine::robotics::workcell;

namespace
{

/** Meters (URDF) -> in-memory millimeters. */
constexpr double kMmPerM = 1000.0;

/**
 * @brief Returns an XML attribute as a String.
 *
 * @param xe The element, or null.
 * @param name The attribute name.
 * @return The value, or empty.
 */
String attr(const tinyxml2::XMLElement* xe, const char* name)
{
    const char* const v = xe ? xe->Attribute(name) : nullptr;
    return v ? String(reinterpret_cast<const char8_t*>(v)) : String();
}

/**
 * @brief Returns a numeric XML attribute with a fallback.
 *
 * @param xe The element, or null.
 * @param name The attribute name.
 * @param def The fallback value.
 * @return The parsed value, or def.
 */
double attrDouble(const tinyxml2::XMLElement* xe, const char* name, double def)
{
    const char* const v = xe ? xe->Attribute(name) : nullptr;
    return v ? std::atof(v) : def;
}

/**
 * @brief Parses a "x y z" space-separated triple.
 *
 * @param str The text.
 * @param out Receives the vector.
 * @return true on success.
 */
bool parseVec3(const String& str, Vec3d& out)
{
    double x = 0.0, y = 0.0, z = 0.0;
    std::istringstream stream(str.as_std_str());
    if (!(stream >> x >> y >> z)) {
        return false;
    }
    out = Vec3d(x, y, z);
    return true;
}

/**
 * @brief Converts URDF rpy (roll/pitch/yaw) to a quaternion.
 *
 * URDF convention: R = Rz(yaw) * Ry(pitch) * Rx(roll).
 *
 * @param roll Rotation about X.
 * @param pitch Rotation about Y.
 * @param yaw Rotation about Z.
 * @return The quaternion.
 */
Quatd rpyToQuat(double roll, double pitch, double yaw)
{
    const double cy = std::cos(yaw * 0.5);
    const double sy = std::sin(yaw * 0.5);
    const double cp = std::cos(pitch * 0.5);
    const double sp = std::sin(pitch * 0.5);
    const double cr = std::cos(roll * 0.5);
    const double sr = std::sin(roll * 0.5);
    const double w  = cr * cp * cy + sr * sp * sy;
    const double x  = sr * cp * cy - cr * sp * sy;
    const double y  = cr * sp * cy + sr * cp * sy;
    const double z  = cr * cp * sy - sr * sp * cy;
    return Quatd(x, y, z, w);
}

/**
 * @brief Parses a URDF <origin xyz rpy> into an isometry, scaled to mm.
 *
 * @param xe_origin The origin element, or null.
 * @return The transform.
 */
Isometry3d parseOrigin(const tinyxml2::XMLElement* xe_origin)
{
    Isometry3d tf; // identity
    if (xe_origin == nullptr) {
        return tf;
    }
    const String xyz = attr(xe_origin, "xyz");
    if (!xyz.empty()) {
        Vec3d t;
        if (parseVec3(xyz, t)) {
            tf.translation = Point3d(t.x * kMmPerM, t.y * kMmPerM, t.z * kMmPerM);
        }
    }
    const String rpy = attr(xe_origin, "rpy");
    if (!rpy.empty()) {
        double r = 0.0, p = 0.0, y = 0.0;
        std::istringstream stream(rpy.as_std_str());
        if (stream >> r >> p >> y) {
            tf.rotation = rpyToQuat(r, p, y);
        }
    }
    return tf;
}

/**
 * @brief Loads an STL/OBJ mesh file into a shape via vine::meshio::MeshLoader.
 *
 * @param loader The mesh loader (scale options preset).
 * @param path The mesh file path.
 * @return The shape, or null on failure.
 */
vine::intrusive_ptr<vine::geometry::Shape> loadShape(vine::meshio::MeshLoader& loader,
                                                     const std::filesystem::path& path)
{
    const auto mesh = loader.load(path);
    if (mesh == nullptr) {
        throw std::runtime_error("failed to load mesh: " + path.string());
    }
    return mesh;
}

/**
 * @brief Parses a URDF <visual> into a Vine visual.
 *
 * Every material is hoisted into the device material library (a name is
 * generated when the URDF leaves it unnamed) and referenced by name; inline
 * materials are not used.
 *
 * @param loader The mesh loader.
 * @param material_lib Device material library (name -> material).
 * @param next_material_id Counter for generating names of unnamed materials.
 * @param xe_visual The visual element.
 * @param base_dir Directory for resolving mesh filenames.
 * @param out The visual to fill.
 */
void parseVisual(vine::meshio::MeshLoader&                                       loader,
                 std::map<String, vine::intrusive_ptr<vine::geometry::Material>>& material_lib,
                 std::size_t&                                                    next_material_id,
                 const tinyxml2::XMLElement* xe_visual, const std::filesystem::path& base_dir,
                 workcell::Visual& out)
{
    out.setTf(parseOrigin(xe_visual->FirstChildElement("origin")));
    const auto* const xe_geom  = xe_visual->FirstChildElement("geometry");
    const auto* const xe_mesh  = xe_geom ? xe_geom->FirstChildElement("mesh") : nullptr;
    if (xe_mesh) {
        const String filename = attr(xe_mesh, "filename");
        if (!filename.empty()) {
            out.setShape(loadShape(loader, base_dir / filename.as_std_str()));
        }
    }
    const auto* const xe_material = xe_visual->FirstChildElement("material");
    if (xe_material) {
        // URDF encodes the color as a child <color rgba="..."/> element.
        String       mat_name = attr(xe_material, "name");
        const String rgba     = attr(xe_material->FirstChildElement("color"), "rgba");
        if (!rgba.empty()) {
            double r = 0.0, g = 0.0, b = 0.0, a = 1.0;
            std::istringstream stream(rgba.as_std_str());
            if (stream >> r >> g >> b >> a) {
                auto material = vine::intrusive_ptr<vine::geometry::Material>(
                    new vine::geometry::ColorMaterial(vine::Colorf(static_cast<float>(r), static_cast<float>(g),
                                                                   static_cast<float>(b), static_cast<float>(a))));
                if (mat_name.empty()) {
                    // Materials must be named; generate one for unnamed ones.
                    const std::string id_str = std::to_string(next_material_id++);
                    mat_name = String(u8"material_")
                             + String(reinterpret_cast<const char8_t*>(id_str.data()), id_str.size());
                }
                const auto it = material_lib.find(mat_name);
                if (it == material_lib.end()) {
                    material_lib.emplace(mat_name, material);
                    out.setMaterial(material);
                }
                else {
                    out.setMaterial(it->second);
                }
                out.setMaterialName(mat_name);
            }
        }
    }
}

/**
 * @brief Parses a URDF <collision> into a Vine collision.
 *
 * @param loader The mesh loader.
 * @param xe_collision The collision element.
 * @param base_dir Directory for resolving mesh filenames.
 * @param out The collision to fill.
 */
void parseCollision(vine::meshio::MeshLoader& loader, const tinyxml2::XMLElement* xe_collision,
                    const std::filesystem::path& base_dir, workcell::Collision& out)
{
    out.setTf(parseOrigin(xe_collision->FirstChildElement("origin")));
    const auto* const xe_geom = xe_collision->FirstChildElement("geometry");
    const auto* const xe_mesh = xe_geom ? xe_geom->FirstChildElement("mesh") : nullptr;
    if (xe_mesh) {
        const String filename = attr(xe_mesh, "filename");
        if (!filename.empty()) {
            out.setShape(loadShape(loader, base_dir / filename.as_std_str()));
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: urdf2vine <device.urdf> [output.vdevpkg]\n";
        return 1;
    }
    const std::filesystem::path urdf_path = argv[1];
    const std::filesystem::path base_dir  = urdf_path.parent_path();

    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(urdf_path.string().c_str()) != tinyxml2::XML_SUCCESS) {
        std::cerr << "failed to load URDF: " << urdf_path.string() << "\n";
        return 1;
    }
    const auto* const xe_robot = doc.FirstChildElement("robot");
    if (xe_robot == nullptr) {
        std::cerr << "no <robot> root element in " << urdf_path.string() << "\n";
        return 1;
    }
    const String robot_name = attr(xe_robot, "name");

    auto data = std::make_unique<MotionDeviceData>();
    data->kind = DeviceKind::Manipulator;
    data->metadata.length_unit = LengthUnit::Millimeter;
    data->metadata.ik_solver_type = kinematics::IKSolverType::Pieper;
    if (const auto* const xe_meta = xe_robot->FirstChildElement("metadata")) {
        data->metadata.id            = attr(xe_meta, "id");
        data->metadata.name          = attr(xe_meta, "name");
        data->metadata.description   = attr(xe_meta, "description");
        data->metadata.sn            = attr(xe_meta, "sn");
        data->metadata.manufacturer  = attr(xe_meta, "manufacturer");
        data->metadata.model         = attr(xe_meta, "model").empty() ? robot_name : attr(xe_meta, "model");
        data->metadata.author        = attr(xe_meta, "author");
        data->metadata.version       = attr(xe_meta, "version");
        data->metadata.create_time   = attr(xe_meta, "create_time");
        data->metadata.modified_time = attr(xe_meta, "modified_time");
    }
    else {
        data->metadata.model = robot_name;
    }

    // Meshes (STL/OBJ) are loaded through vine::meshio::MeshLoader. Auto
    // scaling infers the source unit from the AABB diagonal and converts to
    // mm, so both meter-based and mm-based URDF mesh packages convert
    // correctly (URDF joint origins are meters and scaled separately).
    vine::meshio::MeshLoader mesh_loader;
    mesh_loader.options().scale_mode            = vine::meshio::MeshLoader::ScaleMode::Auto;
    mesh_loader.options().auto_scale_output_unit = vine::meshio::MeshLoader::LengthUnit::Millimeter;

    // Named materials are collected per device and referenced by name.
    std::map<String, vine::intrusive_ptr<vine::geometry::Material>> material_lib;
    std::size_t next_material_id = 0;

    std::map<String, Link*> link_by_name;
    for (const auto* xe_link = xe_robot->FirstChildElement("link"); xe_link;
         xe_link = xe_link->NextSiblingElement("link")) {
        auto link = std::make_unique<Link>(attr(xe_link, "name"));
        for (const auto* xe_visual = xe_link->FirstChildElement("visual"); xe_visual;
             xe_visual = xe_visual->NextSiblingElement("visual")) {
            workcell::Visual visual;
            parseVisual(mesh_loader, material_lib, next_material_id, xe_visual, base_dir, visual);
            link->body().visuals().push_back(std::move(visual));
        }
        for (const auto* xe_collision = xe_link->FirstChildElement("collision"); xe_collision;
             xe_collision = xe_collision->NextSiblingElement("collision")) {
            workcell::Collision collision;
            parseCollision(mesh_loader, xe_collision, base_dir, collision);
            link->body().collisions().push_back(std::move(collision));
        }
        link_by_name[link->name()] = link.get();
        data->links.push_back(std::move(link));
    }

    for (const auto* xe_joint = xe_robot->FirstChildElement("joint"); xe_joint;
         xe_joint = xe_joint->NextSiblingElement("joint")) {
        const String type       = attr(xe_joint, "type");
        const String parent_name = attr(xe_joint->FirstChildElement("parent"), "link");
        const String child_name  = attr(xe_joint->FirstChildElement("child"), "link");

        kinematics::FrameType ftype = kinematics::FrameType::Fixed;
        if (type == u8"revolute") {
            ftype = kinematics::FrameType::RevoluteJoint;
        }
        else if (type == u8"prismatic") {
            ftype = kinematics::FrameType::PrismaticJoint;
        }
        else if (type == u8"planar") {
            ftype = kinematics::FrameType::PlanarJoint;
        }

        auto joint = std::make_unique<Joint>(ftype);
        joint->setName(attr(xe_joint, "name"));
        joint->setParentLink(link_by_name[parent_name]);
        joint->setChildLink(link_by_name[child_name]);
        joint->setFixedTransform(parseOrigin(xe_joint->FirstChildElement("origin")));

        if (ftype != kinematics::FrameType::Fixed) {
            DofInfo dof;
            dof.type = ftype == kinematics::FrameType::PrismaticJoint
                           ? kinematics::DofType::PrismaticJoint
                           : kinematics::DofType::RevoluteJoint;
            Vec3d axis;
            if (parseVec3(attr(xe_joint->FirstChildElement("axis"), "xyz"), axis)) {
                dof.axis = axis;
            }
            const auto* const xe_limit = xe_joint->FirstChildElement("limit");
            dof.lower                = attrDouble(xe_limit, "lower", 0.0);
            dof.upper                = attrDouble(xe_limit, "upper", 0.0);
            dof.velocity_limit       = attrDouble(xe_limit, "velocity", 0.0);
            dof.acceleration_limit   = attrDouble(xe_limit, "acceleration", 0.0);
            joint->setDofInfos({ dof });
        }
        data->joints.push_back(std::move(joint));
    }

    // The device's named material library is built from the URDF materials.
    data->materials.reserve(material_lib.size());
    for (const auto& entry : material_lib) {
        data->materials.push_back(workcell::DeviceMaterial{ entry.first, entry.second });
    }

    auto device = std::make_unique<MotionDevice>();
    try {
        device->init(std::move(data));
    }
    catch (const std::exception& e) {
        std::cerr << "device init failed: " << e.what() << "\n";
        return 1;
    }
    if (!device->isValid()) {
        std::cerr << "device is not valid after init\n";
        return 1;
    }

    const std::filesystem::path out_path = argc >= 3 ? std::filesystem::path(argv[2])
                                                     : base_dir / (robot_name.as_std_str() + ".vdevpkg");

    vine::robotics::io::DeviceIO io;
    io.savePkg(*device, out_path);
    std::cout << "saved: " << out_path.string() << "\n";

    // Verify by loading the package back.
    auto loaded = io.loadPkg(out_path);
    if (loaded == nullptr) {
        std::cerr << "loadPkg verification failed\n";
        return 1;
    }
    const auto* const motion = dynamic_cast<const MotionDevice*>(loaded.get());
    const std::size_t mesh_visuals = [&motion]() {
        std::size_t n = 0;
        if (motion) {
            for (const auto* const link : motion->links()) {
                n += link->body().visuals().size();
            }
        }
        return n;
    }();
    std::cout << "loadPkg OK: kind=" << (loaded->deviceKind() == DeviceKind::Manipulator ? "Manipulator" : "Other")
              << " links=" << loaded->links().size() << " joints=" << loaded->joints().size()
              << " visual_meshes=" << mesh_visuals << "\n";
    return 0;
}
