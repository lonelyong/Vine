#include <vine/vsg/VsgBackendUtility.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <vsg/app/RenderGraph.h>
#include <vsg/nodes/InstrumentationNode.h>

#include <vine/vsg/VsgHostWindow.hpp>
#include <vine/vsg/VsgRendererState.hpp>
#include <vine/vsg/VsgLights.hpp>

V_VSG_NS_BEGIN

namespace detail
{

std::vector<std::size_t> stableTopologicalOrder(std::size_t node_count,
                                                const std::vector<GraphOrderEdge>& edges)
{
    std::vector<std::vector<std::size_t>> consumers(node_count);
    std::vector<std::size_t>              indegree(node_count, 0);
    for (const auto& edge : edges) {
        if (edge.consumer >= node_count || edge.source >= node_count || edge.consumer == edge.source) {
            continue; // defensive: out-of-range / self edge
        }
        consumers[edge.source].push_back(edge.consumer);
        ++indegree[edge.consumer];
    }

    std::vector<std::size_t> order;
    order.reserve(node_count);
    std::vector<std::size_t> ready;
    ready.reserve(node_count);
    for (std::size_t i = 0; i < node_count; ++i) {
        if (indegree[i] == 0) {
            ready.push_back(i); // seeded in index order -> stable
        }
    }
    for (std::size_t head = 0; head < ready.size(); ++head) {
        const std::size_t node = ready[head];
        order.push_back(node);
        for (const std::size_t consumer : consumers[node]) {
            if (--indegree[consumer] == 0) {
                ready.push_back(consumer);
            }
        }
    }
    for (std::size_t i = 0; i < node_count; ++i) {
        if (indegree[i] > 0) {
            order.push_back(i); // cycle remainder (unsupported pattern)
        }
    }
    return order;
}

bool supportsRequiredVulkanVersion(std::uint32_t api_version) noexcept
{
    const std::uint32_t major = VK_API_VERSION_MAJOR(api_version);
    const std::uint32_t minor = VK_API_VERSION_MINOR(api_version);
    const std::uint32_t want_major = VK_API_VERSION_MAJOR(kRequiredVulkanVersion);
    const std::uint32_t want_minor = VK_API_VERSION_MINOR(kRequiredVulkanVersion);
    return major > want_major || (major == want_major && minor >= want_minor);
}

bool onHostWindow(const ::vsg::ref_ptr<::vsg::Window>& window)
{
    return window != nullptr && window.cast<detail::VsgHostWindow>() != nullptr;
}

const ::vsg::RenderGraph* underlyingGraph(const ::vsg::Node* child) noexcept
{
    if (child == nullptr) {
        return nullptr;
    }
    if (const auto* graph = dynamic_cast<const ::vsg::RenderGraph*>(child)) {
        return graph;
    }
    if (const auto* wrapper = dynamic_cast<const ::vsg::InstrumentationNode*>(child)) {
        return dynamic_cast<const ::vsg::RenderGraph*>(wrapper->child.get());
    }
    return nullptr;
}

void removeGraphChild(::vsg::Group* graph, const ::vsg::ref_ptr<::vsg::Node>& node)
{    if (graph == nullptr) {
        return;
    }
    auto& children = graph->children;
    // Matched THROUGH a profiling wrapper (see underlyingGraph): a profiled session's command graph holds
    // wrappers, and a pass being retired must still be found and detached by the graph it stands for.
    children.erase(std::remove_if(children.begin(), children.end(),
                                  [&node](const ::vsg::ref_ptr<::vsg::Node>& child) {
                                      return child.get() == node.get() ||
                                             underlyingGraph(child.get()) == node.get();
                                  }),
                   children.end());
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> declaredBindings(const std::string& source)
{
    // Reads "<name> = <uint>" out of a qualifier's text, or @p fallback when the
    // name is absent (i.e. the GLSL default applies).
    const auto assignment = [](const std::string& text, const char* name, std::uint32_t fallback) {
        const std::size_t at = text.find(name);
        const std::size_t eq = at == std::string::npos ? std::string::npos : text.find('=', at);
        if (eq == std::string::npos) {
            return fallback;
        }
        std::size_t digit = eq + 1;
        while (digit < text.size() && !std::isdigit(static_cast<unsigned char>(text[digit]))) {
            ++digit;
        }
        return digit < text.size() ? static_cast<std::uint32_t>(std::strtoul(text.c_str() + digit, nullptr, 10))
                                   : fallback;
    };

    std::vector<std::pair<std::uint32_t, std::uint32_t>> bindings;
    std::size_t                                          pos = 0;
    while ((pos = source.find("layout", pos)) != std::string::npos) {
        const std::size_t open = source.find('(', pos);
        const std::size_t close = open == std::string::npos ? std::string::npos : source.find(')', open);
        if (close == std::string::npos) {
            break;
        }
        const std::string qualifier = source.substr(open + 1, close - open - 1);
        pos                         = close + 1;
        if (qualifier.find("binding") == std::string::npos) {
            continue; // location / push_constant / ... qualifiers name no descriptor
        }
        bindings.emplace_back(assignment(qualifier, "set", 0u), assignment(qualifier, "binding", 0u));
    }
    return bindings;
}


bool programDeclaresBinding(vine::raw_ptr<const vine::graphics::ShaderProgram> program, std::uint32_t set,
                           std::uint32_t binding)
{
    if (program == nullptr) {
        return false;   // no program: nothing of its own to declare anything
    }
    for (std::size_t i = 0; i < program->stageCount(); ++i) {
        const auto* stage = program->stage(i);
        if (stage == nullptr) {
            continue;
        }
        for (const auto& [declared_set, declared_binding] : declaredBindings(stage->source.as_std_str())) {
            if (declared_set == set && declared_binding == binding) {
                return true;
            }
        }
    }
    return false;
}

bool programImportsDefine(vine::raw_ptr<const vine::graphics::ShaderProgram> program, const std::string& define)
{
    if (program == nullptr || define.empty()) {
        return false;   // no program: nothing of its own to ask for
    }
    for (std::size_t i = 0; i < program->stageCount(); ++i) {
        const auto* stage = program->stage(i);
        if (stage == nullptr) {
            continue;
        }
        const std::string source = stage->source.as_std_str();
        const std::size_t pragma = source.find("import_defines");
        if (pragma == std::string::npos) {
            continue;
        }
        const std::size_t open = source.find('(', pragma);
        const std::size_t close = open == std::string::npos ? std::string::npos : source.find(')', open);
        if (open == std::string::npos || close == std::string::npos) {
            continue;
        }
        // The list is comma-separated: the name has to match a whole entry, or "VINE_A" would be found
        // inside "VINE_AB" and a program would be credited with an opt-in it never wrote.
        const std::string list = source.substr(open + 1, close - open - 1);
        std::size_t       at   = 0;
        while (at < list.size()) {
            while (at < list.size() && !std::isalnum(static_cast<unsigned char>(list[at])) && list[at] != '_') {
                ++at;
            }
            const std::size_t begin = at;
            while (at < list.size() && (std::isalnum(static_cast<unsigned char>(list[at])) || list[at] == '_')) {
                ++at;
            }
            if (at > begin && list.compare(begin, at - begin, define) == 0) {
                return true;
            }
        }
    }
    return false;
}

ShadowInput resolveShadowInput(const VsgRendererState& state, vine::raw_ptr<const vine::graphics::Camera> camera,
                               const std::vector<const vine::graphics::Light*>& lights)
{
    ShadowInput resolved;
    if (camera == nullptr) {
        return resolved; // no view to map a fragment from: nothing can be shaded with a map
    }
    // A shadow map says WHOSE it is (RenderTarget::setShadowOf) and the pass that reads it is a generic
    // consumer: it declares targets as its inputs and nothing else. So the map is found by what the
    // TARGET states, never by declaration order - a G-buffer has a depth too, and "the first declared
    // input whose depth is sampleable" bound one as the sun's map for a whole deferred branch, shading a
    // "shadow" that was a function of world position (see .ai/design/graphics-shadow.md).
    const vine::graphics::RenderTarget* map    = nullptr;
    const vine::graphics::Light*        light  = nullptr;
    const VsgRenderTargetEntry*         chosen = nullptr;
    for (const auto& input : state.request.inputs) {
        if (input == nullptr || input->shadowOf() == nullptr) {
            continue; // this input is not a shadow map: only a map states whose shadow it is
        }
        const auto found = state.targets.find(input);
        if (found == state.targets.end() || found->second.depth_view == nullptr || !found->second.depth_sampleable) {
            continue; // declared but not produced (yet), or its depth cannot be sampled
        }
        map    = input;
        light  = input->shadowOf();
        chosen = &found->second;
        break;
    }
    if (map == nullptr || light == nullptr) {
        return resolved; // no usable shadow map is declared here: the ABI's switch stays off
    }
    // The pass says WHICH shadow it shades; the light says whether it casts one right now (Light owns
    // that switch, together with the ShadowSettings the bias comes from). A light that stopped casting
    // between two frames has to stop shading, or toggling it would change nothing at all.
    if (!light->castShadow()) {
        return resolved;
    }
    // A map is only usable with the matrix its producer published. Without one it would be mapped with
    // the identity, which shades a "shadow" that is a function of world position; shading nothing is the
    // honest answer for a map nobody stated how to read.
    if (!map->hasProducerViewProjection()) {
        return resolved;
    }
    // The light has to be one the block can NAME. The shader's shadow term scales the light whose slot
    // the block states, so a caster the block cannot carry (disabled, or a fourth directional) leaves
    // the switch off rather than scaling a light the map does not belong to.
    const std::size_t slot = directionalSlotOf(lights, light);
    if (slot >= 3u) {
        return resolved;
    }
    // The bias is the casting light's own (ShadowSettings): a private per-backend bias is exactly the
    // convention this ABI exists to prevent.
    const float bias     = static_cast<float>(light->shadowSettings().bias);
    const float strength = 1.0f;
    resolved.map         = chosen->depth_view;
    resolved.source      = map;
    // view -> light clip = (producer: light clip <- light view) * (view <- world) * (world <- THIS
    // view): the producer's view-projection maps ITS view-space position into light clip, and the
    // fragment the shader has is in the consuming pass' view space.
    const vine::math::Mat4d view_to_light = map->producerViewProjection() * camera->viewMatrix().inverted();
    // Column-major, the way the GLSL block reads it (mat4 is four columns of vec4).
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            resolved.block.view_to_light[static_cast<std::size_t>(column * 4 + row)] =
                static_cast<float>(view_to_light(row, column));
        }
    }
    resolved.block.params = { 1.0f, bias, strength, static_cast<float>(slot) };
    return resolved;
}

vine::graphics::Viewport passDrawRect(const std::optional<vine::graphics::Viewport>& viewport, int surf_w, int surf_h)
{
    vine::graphics::Viewport rect{ 0, 0, surf_w, surf_h };
    if (viewport && viewport->width > 0 && viewport->height > 0) {
        rect = *viewport;
    }
    // Clamped into the target: the caller has no auto-fit, and an origin outside it would draw nothing.
    if (rect.x < 0) {
        rect.width += rect.x;
        rect.x = 0;
    }
    if (rect.y < 0) {
        rect.height += rect.y;
        rect.y = 0;
    }
    if (rect.x + rect.width > surf_w) {
        rect.width = surf_w - rect.x;
    }
    if (rect.y + rect.height > surf_h) {
        rect.height = surf_h - rect.y;
    }
    if (rect.width < 0) {
        rect.width = 0;
    }
    if (rect.height < 0) {
        rect.height = 0;
    }
    return rect;
}

} // namespace detail

V_VSG_NS_END
