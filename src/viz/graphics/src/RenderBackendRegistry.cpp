#include <vine/graphics/RenderBackendRegistry.hpp>

#include <map>
#include <mutex>
#include <string>

#include <vine/graphics/RenderBackend.hpp>

#include "EngineDiagnostics.hpp"

VN_GRAPHICS_NS_BEGIN

String renderApiToString(RenderApi api)
{
    if (api == RenderApi::None) {
        return u8"unknown";
    }

    String     result;
    const auto append = [&result](const char8_t* token) {
        if (!result.empty()) {
            result += u8" | ";
        }
        result += token;
    };

    if (vn::testFlag(api, RenderApi::Vulkan)) {
        append(u8"vulkan");
    }
    if (vn::testFlag(api, RenderApi::OpenGL2)) {
        append(u8"opengl2");
    }
    if (vn::testFlag(api, RenderApi::OpenGL3)) {
        append(u8"opengl3");
    }
    if (vn::testFlag(api, RenderApi::OpenGLES)) {
        append(u8"gles");
    }
    if (vn::testFlag(api, RenderApi::Direct3D)) {
        append(u8"dx");
    }
    return result;
}

struct RenderBackendRegistry::Data {
    std::map<String, RenderBackendFactory*> factories;
    std::mutex                              mutex;
    /// The route a refused registration is reported on (see diagnostics()). Registrations arrive from a static
    /// Registrar or a plugin load, so no frame-path report shares it.
    Diagnostics diagnostics;
};

RenderBackendRegistry::RenderBackendRegistry()
  : d(std::make_unique<Data>())
{}

RenderBackendRegistry::~RenderBackendRegistry() = default;

RenderBackendRegistry& RenderBackendRegistry::instance()
{
    static RenderBackendRegistry registry;
    return registry;
}

Diagnostics& RenderBackendRegistry::diagnostics() noexcept
{
    return d->diagnostics;
}

namespace
{

/**
 * @brief Reports one refused registration: counted on the registry's route and forwarded to the host's sink.
 *
 * @details
 * - Origin is Engine because a registry acts for the engine (`DiagnosticOrigin::Engine`), and the severity is RAISED
 *   to Error: the engine's usual builder defaults to Warning, which describes a missing PART of a picture, while a
 *   backend that never joined the process is the whole picture for its name.
 * - No subject: naming the backend would mean calling the module, which is the call this refusal exists to avoid.
 *
 * @param route      The registry's route.
 * @param diagnostic The value the caller built, with its stable code.
 */
void reportRefusal(Diagnostics& route, RenderDiagnostic diagnostic)
{
    diagnostic.severity = DiagnosticSeverity::Error;
    route.report(diagnostic);
}

/**
 * @brief Reports a module that stated no SDK revision at all (see VN_DECLARE_RENDER_BACKEND_ABI).
 *
 * @param route The registry's route.
 */
void reportAbiMissing(Diagnostics& route)
{
    reportRefusal(route,
                  detail::engineProblem(u8"engine.backend.abi_missing",
                                        DiagnosticCategory::InitFailed,
                                        String::fromUtf8("A render backend module states no graphics SDK ABI revision (vineRenderBackendAbi): it was built "
                                                         "against an SDK older than ABI " +
                                                         std::to_string(VN_RENDER_BACKEND_ABI_VERSION) +
                                                         ", which is what this host speaks (SDK " VN_GRAPHICS_VERSION
                                                         "). Rebuild that backend module against this SDK; it was not registered.")));
}

/**
 * @brief Reports a module built against another revision, naming both sides.
 *
 * @param route The registry's route.
 * @param abi   What the module reported (its `sdk_version` may be a null pointer if it is a hand-written one).
 */
void reportAbiMismatch(Diagnostics& route, const RenderBackendAbi& abi)
{
    reportRefusal(
        route,
        detail::engineProblem(u8"engine.backend.abi_mismatch",
                              DiagnosticCategory::InitFailed,
                              String::fromUtf8("A render backend module was built against graphics SDK ABI " + std::to_string(abi.abi_version) + " (SDK " +
                                               (abi.sdk_version != nullptr ? abi.sdk_version : "unknown") + "), but this host speaks ABI " +
                                               std::to_string(VN_RENDER_BACKEND_ABI_VERSION) +
                                               " (SDK " VN_GRAPHICS_VERSION "). Rebuild that backend module against this SDK; it was not registered.")));
}

} // namespace

BackendRegistrationResult RenderBackendRegistry::registerFactory(raw_ptr<RenderBackendFactory> factory, const RenderBackendAbi* abi)
{
    if (factory == nullptr) {
        return BackendRegistrationResult::InvalidFactory;
    }

    // THE MODULE'S BUILD IDENTITY IS CHECKED BEFORE THE MODULE IS ASKED ANYTHING (see VN_RENDER_BACKEND_ABI_VERSION):
    // `factory->name()` is a virtual call through a vtable whose shape belongs to the revision that built the module,
    // so reading it from a stale module is exactly the crash this gate exists to turn into a sentence.
    if (abi == nullptr) {
        reportAbiMissing(d->diagnostics);
        return BackendRegistrationResult::AbiMissing;
    }
    if (!isRenderBackendAbiCompatible(abi)) {
        reportAbiMismatch(d->diagnostics, *abi);
        return BackendRegistrationResult::AbiMismatch;
    }

    const String                      name = factory->name();
    const std::lock_guard<std::mutex> lock(d->mutex);
    d->factories[name] = factory;
    return BackendRegistrationResult::Ok;
}

BackendRegistrationResult RenderBackendRegistry::registerFactory(raw_ptr<RenderBackendFactory> factory)
{
    if (factory == nullptr) {
        return BackendRegistrationResult::InvalidFactory;
    }
    // The compatibility overload (see the header): the module predates the handshake, so there is nothing to compare
    // and nothing to ask - it is refused with the sentence that tells its author what to do.
    reportAbiMissing(d->diagnostics);
    return BackendRegistrationResult::AbiMissing;
}

vn::intrusive_ptr<RenderBackend> RenderBackendRegistry::create(const String& name, const DeviceDesc& desc) const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    const auto                  it = d->factories.find(name);
    if (it == d->factories.end()) {
        return {};
    }
    return it->second->create(desc);
}

std::vector<String> RenderBackendRegistry::names() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    std::vector<String>         result;
    result.reserve(d->factories.size());
    for (const auto& [name, _] : d->factories) {
        result.push_back(name);
    }
    return result;
}

std::vector<RenderBackendRegistry::Entry> RenderBackendRegistry::entries() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    std::vector<Entry>          result;
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

VN_GRAPHICS_NS_END
