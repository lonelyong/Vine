#include <vine/graphics/DrawState.hpp>

#include <array>

VN_GRAPHICS_NS_BEGIN

bool VertexLayoutKey::operator==(const VertexLayoutKey& other) const noexcept
{
    return slots == other.slots;
}

std::size_t VertexLayoutKeyHash::operator()(const VertexLayoutKey& key) const noexcept
{
    std::size_t hash = 0;
    const auto  mix  = [&hash](std::uint64_t value) noexcept { hash ^= static_cast<std::size_t>(value) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2); };
    for (const VertexLayoutSlot& slot : key.slots) {
        mix(slot.location);
        mix(slot.components);
    }
    return hash;
}

bool DeviceFormat::operator==(const DeviceFormat& other) const noexcept
{
    return code == other.code;
}

bool DeviceFormat::operator<(const DeviceFormat& other) const noexcept
{
    return code < other.code;
}

bool RenderPassKey::operator==(const RenderPassKey& other) const noexcept
{
    return bits == other.bits;
}

bool RenderPassKey::operator<(const RenderPassKey& other) const noexcept
{
    return bits < other.bits;
}

bool RenderPassCompatibility::operator==(const RenderPassCompatibility& other) const noexcept
{
    return color_formats == other.color_formats && depth_format == other.depth_format && device_color_formats == other.device_color_formats &&
           device_depth_format == other.device_depth_format && samples == other.samples && render_pass_key == other.render_pass_key;
}

bool PipelineKey::operator==(const PipelineKey& other) const noexcept
{
    return kind == other.kind && program == other.program && vertex_layout == other.vertex_layout &&
           compatibility == other.compatibility && depth_sampleable == other.depth_sampleable && sampled_color_count == other.sampled_color_count &&
           sampled_depth_count == other.sampled_depth_count && variant == other.variant && topology == other.topology;
}

bool DynamicState::operator==(const DynamicState& other) const noexcept
{
    return depth == other.depth && compare == other.compare && cull_mode == other.cull_mode && polygon_mode == other.polygon_mode &&
           polygon_offset == other.polygon_offset && topology == other.topology && line_width == other.line_width && blend == other.blend;
}

DynamicState resolveDynamicState(const vn::graphics::ResolvedCommandState& command) noexcept
{
    const vn::graphics::ResolvedRenderState& state = command.state;
    DynamicState                             resolved;
    resolved.depth =
        state.depth.test ? (state.depth.write ? vn::graphics::DepthPolicy::TestAndWrite : vn::graphics::DepthPolicy::TestOnly) : vn::graphics::DepthPolicy::Disabled;
    resolved.compare        = state.depth.compare;
    resolved.cull_mode      = state.cullMode;
    resolved.polygon_mode   = state.polygonMode;
    resolved.polygon_offset = state.polygonOffset;
    resolved.topology       = command.topology;
    resolved.line_width     = state.lineWidth;
    resolved.blend          = state.blend;
    return resolved;
}

std::size_t PipelineKeyHash::operator()(const PipelineKey& key) const noexcept
{
    // The same combine the stream registry uses, over exactly the fields PipelineKey::operator== reads.
    std::size_t hash = 0;
    const auto  mix  = [&hash](std::uint64_t value) noexcept { hash ^= static_cast<std::size_t>(value) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2); };
    mix(key.program.id.bits());
    mix(key.program.revision);
    mix(static_cast<std::uint64_t>(key.kind));
    for (const VertexLayoutSlot& slot : key.vertex_layout.slots) {
        mix(slot.location);
        mix(slot.components);
    }
    for (const auto format : key.compatibility.color_formats) {
        mix(static_cast<std::uint64_t>(format));
    }
    mix(key.compatibility.depth_format.has_value() ? static_cast<std::uint64_t>(*key.compatibility.depth_format) + 1U : 0U);
    for (const DeviceFormat format : key.compatibility.device_color_formats) {
        mix(format.code);
    }
    mix(key.compatibility.device_depth_format.code);
    mix(key.compatibility.samples);
    mix(key.compatibility.render_pass_key.bits);
    mix(key.depth_sampleable ? 1U : 0U);
    mix(key.sampled_color_count);
    mix(key.sampled_depth_count);
    mix(key.variant);
    mix(static_cast<std::uint64_t>(key.topology));
    return hash;
}

namespace detail
{

std::span<const KeyAuditEntry> keyAuditTable() noexcept
{
    // The audit, as data. Each line answers "what may enter this key?" - and the answer for anything
    // that can be delivered with a set command, or written into a buffer, is "nothing".
    static constexpr std::array<KeyAuditEntry, 6> kAudit{
        {
         { "VertexLayoutKey", "one slot per declared attribute, IN BINDING ORDER: its location and its component count (the format) - \"which channels\" alone would share a pipeline across two widths" },
         { "RenderPassCompatibility",
            "colour formats + depth format (or none) + the same attachments as OPAQUE DEVICE format codes + samples "
            "+ the backend's render-pass key (the pass structure)" },
         { "PipelineKey",
            "program IDENTITY + revision + draw kind (content or full-screen) + vertex layout + compatibility + depth "
            "sampleability + sampled colour attachment count + sampled DEPTH count + the program VARIANT (the "
            "defines that change what its text means - see api/ProgramVariant) + the topology CLASS (a static "
            "topology may only be changed dynamically within its own class, so it is compiled in)" },
         { "DynamicState",
            "depth policy + compare + cull + polygon + topology + blend - delivered per draw with set commands (the "
            "topology within the class its pipeline was compiled for, see PipelineKey)" },
         { "InstanceRecord", "model matrix + opacity + material identity + revision - per frame data" },
         { "TargetDesc.shape",
            "attachment formats (the engine's spelling AND the device's opaque codes) + depth format + samples + "
            "the backend's render-pass key - NEVER an extent" },
         }
    };
    return kAudit;
}

} // namespace detail

VN_GRAPHICS_NS_END
