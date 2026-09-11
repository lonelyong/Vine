#include <vine/graphics/RenderBackendRegistry.hpp>

#include <map>
#include <mutex>

#include <vine/graphics/RenderBackend.hpp>

V_GRAPHICS_NS_BEGIN

String renderApiToString(RenderApi api)
{
    if (api == RenderApi::None) {
        return u8"unknown";
    }

    String result;
    const auto append = [&result](const char8_t* token) {
        if (!result.empty()) {
            result += u8" | ";
        }
        result += token;
    };

    if (vine::testFlag(api, RenderApi::Vulkan)) {
        append(u8"vulkan");
    }
    if (vine::testFlag(api, RenderApi::OpenGL2)) {
        append(u8"opengl2");
    }
    if (vine::testFlag(api, RenderApi::OpenGL3)) {
        append(u8"opengl3");
    }
    if (vine::testFlag(api, RenderApi::OpenGLES)) {
        append(u8"gles");
    }
    if (vine::testFlag(api, RenderApi::Direct3D)) {
        append(u8"dx");
    }
    return result;
}

struct RenderBackendRegistry::Data {
    std::map<String, RenderBackendFactory*> factories;
    std::mutex mutex;
};

RenderBackendRegistry::RenderBackendRegistry()
  : d(std::make_unique<Data>())
{
}

RenderBackendRegistry::~RenderBackendRegistry() = default;

RenderBackendRegistry& RenderBackendRegistry::instance()
{
    static RenderBackendRegistry registry;
    return registry;
}

void RenderBackendRegistry::registerFactory(raw_ptr<RenderBackendFactory> factory)
{
    if (factory == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(d->mutex);
    d->factories[factory->name()] = factory;
}

vine::intrusive_ptr<RenderBackend> RenderBackendRegistry::create(const String& name) const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    const auto it = d->factories.find(name);
    if (it == d->factories.end()) {
        return {};
    }
    return it->second->create();
}

std::vector<String> RenderBackendRegistry::names() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    std::vector<String> result;
    result.reserve(d->factories.size());
    for (const auto& [name, _] : d->factories) {
        result.push_back(name);
    }
    return result;
}

std::vector<RenderBackendRegistry::Entry> RenderBackendRegistry::entries() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    std::vector<Entry> result;
    result.reserve(d->factories.size());
    for (const auto& [name, factory] : d->factories) {
        result.push_back(Entry{ factory->info(), factory });
    }
    return result;
}

bool RenderBackendRegistry::has(const String& name) const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->factories.find(name) != d->factories.end();
}

V_GRAPHICS_NS_END
