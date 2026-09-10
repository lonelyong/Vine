#include <vine/appfw/PluginLoadContext.hpp>

#include <filesystem>
#include <system_error>
#include <utility>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/ConfigRegistry.hpp>
#include <vine/logging/Log.hpp>

V_APPFW_NS_BEGIN

namespace
{

/// Converts a String to a std::string for fmt-based logging.
std::string toUtf8(const String& s)
{
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

} // namespace

struct PluginLoadContext::Impl {
    Application* app = nullptr;
    String       plugin_name;
};

PluginLoadContext::PluginLoadContext(Application* app, String plugin_name)
  : d(new Impl)
{
    d->app         = app;
    d->plugin_name = std::move(plugin_name);
}

PluginLoadContext::~PluginLoadContext() = default;

raw_ptr<Application> PluginLoadContext::application() const
{
    return d->app;
}

raw_ptr<ConfigRegistry> PluginLoadContext::configs() const
{
    return d->app ? d->app->configRegistry() : nullptr;
}

raw_ptr<CommandManager> PluginLoadContext::commandManager() const
{
    return d->app ? d->app->commandManager() : nullptr;
}

raw_ptr<EventBus> PluginLoadContext::eventBus() const
{
    return d->app ? d->app->eventBus() : nullptr;
}

const String& PluginLoadContext::pluginName() const
{
    return d->plugin_name;
}

std::filesystem::path PluginLoadContext::dataDirectory()
{
    if (d->app == nullptr || d->plugin_name.empty()) {
        return {};
    }

    // Keyed by the plugin name so the same plugin keeps its files when its
    // library is moved (e.g. a user-installed plugin living outside the
    // application directory).
    const std::u8string name(d->plugin_name);
    std::filesystem::path dir = d->app->pluginDataDirectory() / std::filesystem::path(name);

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        V_LOGW("Could not create the data directory '{}' for plugin '{}': {}", dir.string(), toUtf8(d->plugin_name), ec.message());
        return {};
    }
    return dir;
}

bool PluginLoadContext::registerConfigItem(StandardCategory cat, StandardGroup grp, const ConfigItem& item)
{
    auto* reg = configs();
    return reg ? reg->addItem(cat, grp, item, d->plugin_name) : false;
}

std::vector<const ConfigItem*> PluginLoadContext::registeredConfigs() const
{
    auto* reg = configs();
    return reg ? reg->itemsForPlugin(d->plugin_name) : std::vector<const ConfigItem*>{};
}

V_APPFW_NS_END
