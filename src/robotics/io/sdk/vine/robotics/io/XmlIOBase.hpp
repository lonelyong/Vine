#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/math/Isometry3.hpp>
#include <vine/robotics/io/robot_io_global.hpp>
#include <vine/String.hpp>
#include <vine/robotics/workcell/Collision.hpp>
#include <vine/robotics/workcell/Device.hpp>
#include <vine/robotics/workcell/Joint.hpp>
#include <vine/robotics/workcell/Link.hpp>
#include <vine/robotics/workcell/Visual.hpp>

namespace tinyxml2
{
class XMLDocument;
class XMLElement;
} // namespace tinyxml2

namespace vn::io
{
class Vfs;
} // namespace vn::io

VN_ROBOTICS_IO_NS_BEGIN

/**
 * @brief Shared parse/export helpers for the XML device and workcell formats.
 *
 * Holds the parse/export contexts (length-unit scaling), version handling,
 * pose/geometry/material serialization and the per-link / per-joint code
 * shared by DeviceIO and WorkcellIO. Warnings are collected into the active
 * context instead of failing; fatal errors are thrown as std::runtime_error.
 *
 * The base class is stateless: every per-operation value lives in the
 * parse/export context, so IO instances are reentrant and safe to reuse.
 */
class VN_ROBOTICS_IO_API XmlIOBase
{
  protected:
    /**
     * @brief Export options.
     */
    struct ExportOptions
    {
        /// Length-unit scale applied to exported lengths.
        double len_unit_scaling = 1.0;
    };

    /**
     * @brief Export context carried through an export pass.
     */
    struct ExportContext
    {
        ExportOptions& options;
        /// Workcell being exported (read-only frame lookups); null otherwise.
        const workcell::Workcell* cell{ nullptr };
        /// Active VFS for package resources (mesh bins); null for bare XML export.
        vn::io::Vfs* vfs{ nullptr };
        /// Directory of the exported document inside the VFS ("" = root).
        std::filesystem::path vfs_dir;
        /// Collected non-fatal warnings.
        std::string msgs;
        /// Sequence number used to generate unique geoms bin names.
        std::size_t geom_seq{ 0 };
        /// Shape -> geoms prefix, so a shared mesh is stored only once.
        std::map<const vn::geometry::Shape*, String> mesh_paths;

        explicit ExportContext(ExportOptions& opts)
          : options(opts)
        {}
    };

    /**
     * @brief Parse options.
     */
    struct ParseOptions
    {
        /// Length-unit scale applied to parsed lengths.
        double len_unit_scaling = 1.0;
    };

    /**
     * @brief Parse context carried through a parse pass.
     */
    struct ParseContext
    {
        ParseOptions& options;
        /// Workcell being filled; null outside a workcell parse.
        workcell::Workcell* cell{ nullptr };
        /// Active VFS for package resources (mesh bins); null for bare XML parse.
        vn::io::Vfs* vfs{ nullptr };
        /// Directory of the parsed document inside the VFS ("" = root).
        std::filesystem::path vfs_dir;
        /// Collected non-fatal warnings.
        std::string msgs;
        /// Device material library by name, for resolving visual references.
        std::map<String, vn::intrusive_ptr<vn::geometry::Material>> materials_by_name;

        explicit ParseContext(ParseOptions& opts)
          : options(opts)
        {}
    };

  public:
    /**
     * @brief Destroys the IO base.
     */
    virtual ~XmlIOBase();

  protected:
    // ---- version ----
    /**
     * @brief Parses a "major.minor" version attribute.
     *
     * @param ctx Parse context.
     * @param major Receives the major version.
     * @param minor Receives the minor version.
     * @param xe The element with the version attribute.
     */
    void parseVersion(ParseContext& ctx, uint16_t& major, uint16_t& minor, const tinyxml2::XMLElement* xe);

    /**
     * @brief Writes a "major.minor" version attribute.
     *
     * @param ctx Export context.
     * @param major The major version.
     * @param minor The minor version.
     * @param xe The element to write the attribute on.
     */
    void exportVersion(ExportContext& ctx, uint16_t major, uint16_t minor, tinyxml2::XMLElement* xe);

    // ---- pose ----
    /**
     * @brief Parses an <origin xyz quat> pose.
     *
     * @param ctx Parse context.
     * @param pose Receives the transform.
     * @param xe The origin element.
     */
    void parsePose(ParseContext& ctx, math::Isometry3d& pose, const tinyxml2::XMLElement* xe);

    /**
     * @brief Writes an <origin xyz quat> pose.
     *
     * @param ctx Export context.
     * @param pose The transform.
     * @param xe The element to append the origin to.
     */
    void exportPose(ExportContext& ctx, const math::Isometry3d& pose, tinyxml2::XMLElement* xe);

    // ---- geometry / material ----
    /**
     * @brief Parses a material element.
     *
     * @param ctx Parse context.
     * @param xe The material element.
     * @return The material, or null when unset.
     */
    vn::intrusive_ptr<vn::geometry::Material> parseMaterial(ParseContext& ctx, const tinyxml2::XMLElement* xe);

    /**
     * @brief Parses a <geometry> element.
     *
     * @param ctx Parse context.
     * @param xe The geometry element.
     * @return The shape, or null when empty or unsupported.
     */
    vn::intrusive_ptr<vn::geometry::Shape> parseGeometry(ParseContext& ctx, const tinyxml2::XMLElement* xe);

    /**
     * @brief Writes a <geometry> element.
     *
     * @param ctx Export context.
     * @param shape The shape.
     * @param xe The element to append the geometry to.
     */
    void exportGeometry(ExportContext& ctx, const vn::geometry::Shape& shape, tinyxml2::XMLElement* xe);

    // ---- visual / collision ----
    /**
     * @brief Parses a <visual> element into a visual.
     *
     * @param ctx Parse context.
     * @param visual The visual to fill.
     * @param xe The visual element.
     */
    void parseVisual(ParseContext& ctx, workcell::Visual& visual, const tinyxml2::XMLElement* xe);

    /**
     * @brief Writes a visual as a <visual> element.
     *
     * @param ctx Export context.
     * @param visual The visual.
     * @param xe The element to append the visual to.
     */
    void exportVisual(ExportContext& ctx, const workcell::Visual& visual, tinyxml2::XMLElement* xe);

    /**
     * @brief Parses a <collision> element into a collision.
     *
     * @param ctx Parse context.
     * @param collision The collision to fill.
     * @param xe The collision element.
     */
    void parseCollision(ParseContext& ctx, workcell::Collision& collision, const tinyxml2::XMLElement* xe);

    /**
     * @brief Writes a collision as a <collision> element.
     *
     * @param ctx Export context.
     * @param collision The collision.
     * @param xe The element to append the collision to.
     */
    void exportCollision(ExportContext& ctx, const workcell::Collision& collision, tinyxml2::XMLElement* xe);

    // ---- link / joint ----
    /**
     * @brief Parses a <link> element into a link.
     *
     * @param ctx Parse context.
     * @param link The link to fill.
     * @param xe The link element.
     */
    void parseLink(ParseContext& ctx, workcell::Link& link, const tinyxml2::XMLElement* xe);

    /**
     * @brief Writes a link as a <link> element.
     *
     * @param ctx Export context.
     * @param link The link.
     * @param xe The element to append the link to.
     */
    void exportLink(ExportContext& ctx, const workcell::Link& link, tinyxml2::XMLElement* xe);

    /**
     * @brief Parses a <joint> element into a joint.
     *
     * Parent/child links are resolved against the given name lookup.
     *
     * @param ctx Parse context.
     * @param xe The joint element.
     * @param link_by_name Name to link lookup.
     * @return The joint, or null when the element is invalid.
     */
    std::unique_ptr<workcell::Joint> parseJoint(ParseContext& ctx,
                                                tinyxml2::XMLElement*      xe,
                                                const std::map<String, raw_ptr<workcell::Link>>& link_by_name);

    /**
     * @brief Writes a joint as a <joint> element.
     *
     * @param ctx Export context.
     * @param joint The joint.
     * @param xe The element to append the joint to.
     */
    void exportJoint(ExportContext& ctx, const workcell::Joint& joint, tinyxml2::XMLElement* xe);

    // ---- device metadata ----
    /**
     * @brief Parses a <metadata> element into device metadata.
     *
     * @param ctx Parse context.
     * @param metadata The metadata to fill.
     * @param xe The metadata element.
     */
    void parseDeviceMetadata(ParseContext& ctx, workcell::DeviceMetadata& metadata, const tinyxml2::XMLElement* xe);

    /**
     * @brief Writes device metadata as a <metadata> element.
     *
     * @param ctx Export context.
     * @param metadata The metadata.
     * @param xe The element to append the metadata to.
     */
    void exportDeviceMetadata(ExportContext& ctx, const workcell::DeviceMetadata& metadata, tinyxml2::XMLElement* xe);
};

VN_ROBOTICS_IO_NS_END
