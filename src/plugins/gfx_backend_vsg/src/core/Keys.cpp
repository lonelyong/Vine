#include <vine/vsg/core/Keys.hpp>

#include <array>

V_VSG_NS_BEGIN

namespace core
{

bool DataKey::operator==(const DataKey& other) const noexcept
{
    return buffer == other.buffer && revision == other.revision && components == other.components &&
           offset == other.offset && count == other.count;
}

bool VertexLayoutKey::operator==(const VertexLayoutKey& other) const noexcept
{
    return canonical_mask == other.canonical_mask && custom_locations == other.custom_locations;
}

bool RenderPassCompatibility::operator==(const RenderPassCompatibility& other) const noexcept
{
    return color_formats == other.color_formats && depth_format == other.depth_format &&
           samples == other.samples && subpass == other.subpass;
}

bool LoadOpVariantKey::operator==(const LoadOpVariantKey& other) const noexcept
{
    return color_load == other.color_load && color_store == other.color_store &&
           depth_load == other.depth_load && depth_store == other.depth_store &&
           initial == other.initial && final == other.final;
}

bool PipelineKey::operator==(const PipelineKey& other) const noexcept
{
    return kind == other.kind && program == other.program && revision == other.revision &&
           vertex_layout == other.vertex_layout && compatibility == other.compatibility &&
           depth_sampleable == other.depth_sampleable && shadow_bound == other.shadow_bound &&
           sampled_color_count == other.sampled_color_count;
}

bool DynamicState::operator==(const DynamicState& other) const noexcept
{
    return depth == other.depth && cull_mode == other.cull_mode && polygon_mode == other.polygon_mode &&
           topology == other.topology && blend == other.blend;
}

DynamicState resolveDynamicState(const vine::graphics::ResolvedRenderState& state, bool depth_explicit,
                                 vine::graphics::DepthMode pass_depth) noexcept
{
    DynamicState resolved;
    resolved.cull_mode    = state.cullMode;
    resolved.polygon_mode = state.polygonMode;
    resolved.topology     = state.topology;
    resolved.blend        = state.blend;

    // The command's own depth state wins where it has one; where it does not, the pass decides for the
    // content it draws (see the declaration for the two pictures the wrong choice produces).
    if (!depth_explicit)
    {
        resolved.depth = pass_depth;
        return resolved;
    }
    if (!state.depth.test)
    {
        resolved.depth = vine::graphics::DepthMode::Disabled;
    }
    else
    {
        resolved.depth = state.depth.write ? vine::graphics::DepthMode::TestAndWrite
                                           : vine::graphics::DepthMode::TestOnly;
    }
    return resolved;
}

std::size_t PipelineKeyHash::operator()(const PipelineKey& key) const noexcept
{
    // The same combine the stream registry uses, over exactly the fields PipelineKey::operator== reads.
    std::size_t hash = 0;
    const auto  mix  = [&hash](std::uint64_t value) noexcept {
        hash ^= static_cast<std::size_t>(value) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    };
    mix(reinterpret_cast<std::uintptr_t>(key.program));
    mix(key.revision);
    mix(static_cast<std::uint64_t>(key.kind));
    mix(key.vertex_layout.canonical_mask);
    for (const std::uint32_t location : key.vertex_layout.custom_locations) {
        mix(location);
    }
    for (const auto format : key.compatibility.color_formats) {
        mix(static_cast<std::uint64_t>(format));
    }
    mix(key.compatibility.depth_format.has_value() ? static_cast<std::uint64_t>(*key.compatibility.depth_format) + 1U : 0U);
    mix(key.compatibility.samples);
    mix(key.compatibility.subpass);
    mix(key.depth_sampleable ? 1U : 0U);
    mix(key.shadow_bound ? 1U : 0U);
    mix(key.sampled_color_count);
    return hash;
}

std::span<const KeyAuditEntry> keyAuditTable() noexcept
{
    // The audit, as data. Each line answers "what may enter this key?" - and the answer for anything
    // that can be delivered with a set command, or written into a buffer, is "nothing".
    static constexpr std::array<KeyAuditEntry, 8> kAudit{ {
        { "DataKey", "buffer identity + upstream revision + channel slice (components/offset/count)" },
        { "VertexLayoutKey", "canonical channel mask + custom channel locations" },
        { "RenderPassCompatibility", "colour formats + depth format (or none) + samples + subpass" },
        { "LoadOpVariantKey",
          "load/store ops + initial/final layout - NOT pipeline identity: compatibility excludes them" },
        { "PipelineKey",
          "program + revision + draw kind (content or full-screen) + vertex layout + compatibility + depth "
          "sampleability + shadow bound + sampled colour attachment count" },
        { "DynamicState",
          "depth policy + cull + polygon + topology + blend - delivered per draw with set commands" },
        { "InstanceSlot", "model matrix + opacity + material identity + revision - per frame data" },
        { "TargetDesc.shape", "attachment formats + depth format + samples + subpass - NEVER an extent" },
    } };
    return kAudit;
}

}  // namespace core

V_VSG_NS_END
