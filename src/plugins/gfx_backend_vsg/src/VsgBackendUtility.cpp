#include <vine/vsg/VsgBackendUtility.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <vine/vsg/VsgHostWindow.hpp>
#include <vine/vsg/VsgRendererState.hpp>

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

bool onHostWindow(const ::vsg::ref_ptr<::vsg::Window>& window)
{
    return window != nullptr && window.cast<detail::VsgHostWindow>() != nullptr;
}

void removeGraphChild(::vsg::Group* graph, const ::vsg::ref_ptr<::vsg::Node>& node)
{
    if (graph == nullptr) {
        return;
    }
    auto& children = graph->children;
    children.erase(
        std::remove_if(children.begin(),
                       children.end(),
                       [&node](const ::vsg::ref_ptr<::vsg::Node>& child) { return child.get() == node.get(); }),
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
    const vine::graphics::RenderTarget* source = nullptr;
    for (const auto& input : state.request.inputs) {
        if (input == nullptr) {
            continue;
        }
        const auto entry = state.targets.find(input);
        if (entry == state.targets.end() || entry->second.depth_view == nullptr ||
            !entry->second.depth_sampleable) {
            continue; // declared but not produced, or its depth is not sampleable: nothing to bind
        }
        resolved.map = entry->second.depth_view;
        source       = input;
        break;
    }
    if (source == nullptr) {
        return resolved;
    }
    // The bias and the strength come from the light that casts it: the same ShadowSettings the
    // pipeline framed its light camera with (a private per-backend bias is exactly the convention the
    // L1 ABI exists to prevent). The first enabled shadow-casting light is the one whose pass was
    // built; with none announced the block stays DISABLED, which is the honest answer for a map that
    // arrived without the light it belongs to.
    float bias     = 0.002f;
    float strength = 1.0f;
    bool  have_light = false;
    for (const auto* light : lights) {
        if (light != nullptr && light->isEnabled() && light->castShadow()) {
            bias       = static_cast<float>(light->shadowSettings().bias);
            have_light = true;
            break;
        }
    }
    if (!have_light) {
        return resolved;
    }
    // view -> light clip = (producer: light clip <- light view) * (view <- world) * (world <- THIS
    // view): the producer's view-projection maps ITS view-space position into light clip, and the
    // fragment the shader has is in the consuming pass' view space.
    const vine::math::Mat4d view_to_light = source->producerViewProjection() * camera->viewMatrix().inverted();
    // Column-major, the way the GLSL block reads it (mat4 is four columns of vec4).
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            resolved.block.view_to_light[static_cast<std::size_t>(column * 4 + row)] =
                static_cast<float>(view_to_light(row, column));
        }
    }
    resolved.block.params = { 1.0f, bias, strength, 0.0f };
    return resolved;
}

} // namespace detail

V_VSG_NS_END
