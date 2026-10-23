#include <vine/graphics/StateResolution.hpp>

VN_GRAPHICS_NS_BEGIN

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

    return resolved;
}

VN_GRAPHICS_NS_END
