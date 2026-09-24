#include <vine/vsg/VsgBackendUtility.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <vsg/app/RenderGraph.h>
#include <vsg/nodes/InstrumentationNode.h>

#include <vine/vsg/VsgHostWindow.hpp>

#include <vsg/vk/Instance.h>
#include <vsg/vk/InstanceExtensions.h>

VN_VSG_NS_BEGIN

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


bool programDeclaresBinding(vn::raw_ptr<const vn::graphics::ShaderProgram> program, std::uint32_t set,
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

bool programImportsDefine(vn::raw_ptr<const vn::graphics::ShaderProgram> program, const std::string& define)
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

bool nameVulkanObject(const ::vsg::Device& device, std::uint64_t handle, VkObjectType type, const char* name) noexcept
{
    const ::vsg::Instance* instance = device.getInstance();
    if (instance == nullptr || handle == 0U || name == nullptr) {
        return false;
    }
    const ::vsg::InstanceExtensions* extensions = instance->getExtensions();
    if (extensions == nullptr || extensions->vkSetDebugUtilsObjectNameEXT == nullptr) {
        return false;  // this instance cannot name objects: nothing to report, nothing to do
    }
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType   = type;
    info.objectHandle = handle;
    info.pObjectName  = name;
    return extensions->vkSetDebugUtilsObjectNameEXT(device.vk(), &info) == VK_SUCCESS;
}

vn::graphics::Viewport passDrawRect(const std::optional<vn::graphics::Viewport>& viewport, int surf_w, int surf_h)
{
    vn::graphics::Viewport rect{ 0, 0, surf_w, surf_h };
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

VN_VSG_NS_END
