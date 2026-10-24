#include <vine/graphics/StateResolution.hpp>

#include <algorithm>

VN_GRAPHICS_NS_BEGIN

namespace
{

/// @brief The smallest point the engine will ask for.
///
/// A point is at least one sample, and the floor is what keeps "0" from becoming a second way to hide
/// geometry (opacity is the one transparency channel - see BlendState). It also sits at the bottom of every
/// implementation's `pointSizeRange`, so the value the shader writes is always one the device accepts.
constexpr float kMinimumPointSize = 1.0F;

}  // namespace

ResolvedRenderState resolveRenderState(const RenderState& state)
{
    ResolvedRenderState resolved;
    if (state.depth) {
        resolved.depth = *state.depth;
    }
    if (state.cullMode) {
        resolved.cullMode = *state.cullMode;
    }
    if (state.blend) {
        resolved.blend = *state.blend;
    }
    if (state.polygonMode) {
        resolved.polygonMode = *state.polygonMode;
    }
    if (state.polygonOffset) {
        resolved.polygonOffset = *state.polygonOffset;
    }
    if (state.pointSize) {
        resolved.pointSize = std::max(*state.pointSize, kMinimumPointSize);
    }
    return resolved;
}

ResolvedCommandState resolveRenderState(const RenderState& authored, const PassRenderState& pass,
                                        Topology data_topology)
{
    ResolvedCommandState resolved;

    // The content's word first, item by item: what a StateNode authored is kept (and says so), everything it did
    // not author is still the "nobody said" case the defaults below answer.
    if (authored.cullMode) {
        resolved.state.cullMode = *authored.cullMode;
        resolved.origin.cullMode = StateOrigin::Content;
    }
    if (authored.blend) {
        resolved.state.blend = *authored.blend;
        resolved.origin.blend = StateOrigin::Content;
    }
    if (authored.polygonMode) {
        resolved.state.polygonMode = *authored.polygonMode;
        resolved.origin.polygonMode = StateOrigin::Content;
    }
    // A depth bias is something the CONTENT says about its own surfaces (this decal sits on that panel), so it
    // has no pass layer: nothing a view chooses can make two coplanar faces stop fighting without also changing
    // what the content means.
    if (authored.polygonOffset) {
        resolved.state.polygonOffset = *authored.polygonOffset;
        resolved.origin.polygonOffset = StateOrigin::Content;
    }
    // The point size is content-authored state like the rest, and it is the one item whose destination is the
    // DRAW BLOCK rather than a pipeline or its dynamic state (see ResolvedRenderState::pointSize). The floor
    // applies wherever the value comes from, so a pass cannot ask for a zero-size point either.
    if (authored.pointSize) {
        resolved.state.pointSize  = std::max(*authored.pointSize, kMinimumPointSize);
        resolved.origin.pointSize = StateOrigin::Content;
    }
    // Topology: the DATA's own word (a primitive's topology) - no StateNode layer exists for it, because a
    // subtree cannot reinterpret another object's vertex/index data. The pass may override it below, which is
    // the "show me this scene as points" view switch.
    resolved.state.topology  = data_topology;
    resolved.origin.topology = StateOrigin::Data;

    // Depth: the pass states a POLICY, the content may state an intent. An authored depth wins (finer-grained
    // intent over the pass default); where the content authored none, the pass decides whether depth is used at
    // all - and the COMPARE stays whatever the content resolved to (the pass has no opinion about it, see
    // PassRenderState).
    if (authored.depth) {
        resolved.state.depth        = *authored.depth;
        resolved.origin.depth       = StateOrigin::Content;
    }
    else {
        resolved.state.depth.test    = pass.depth != DepthMode::Disabled;
        resolved.state.depth.write   = pass.depth == DepthMode::TestAndWrite;
        resolved.origin.depth        = StateOrigin::Pass;
    }

    // Rasterisation is an OVERRIDE, so it comes last and wins even over a content-authored mode: it is the
    // "show me the edges" switch, and a switch a content-authored Line could veto would never take the picture
    // back to Fill (see PassRenderState).
    if (pass.polygonMode) {
        resolved.state.polygonMode = *pass.polygonMode;
        resolved.origin.polygonMode = StateOrigin::Pass;
    }

    // Same rule for the more drastic view switch: a pass that says "assemble everything as points" wins over
    // what the data states (and the pipeline is then compiled for that topology - see core::PipelineKey).
    if (pass.topology) {
        resolved.state.topology  = *pass.topology;
        resolved.origin.topology = StateOrigin::Pass;
    }

    // And the size those points are drawn at: the same viewing-switch rule as the topology above, because
    // "show me this scene as points" is useless at one pixel (see PassRenderState::pointSize).
    if (pass.pointSize) {
        resolved.state.pointSize  = std::max(*pass.pointSize, kMinimumPointSize);
        resolved.origin.pointSize = StateOrigin::Pass;
    }

    return resolved;
}

VN_GRAPHICS_NS_END
