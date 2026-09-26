#include <vine/appfw/Plugin.hpp>

#include <utility>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/ConfigRegistry.hpp>

VN_APPFW_NS_BEGIN

VN_OBJECT_META_IMPL(Plugin, Object)

const PluginInfo& Plugin::info() const
{
    return info_;
}

void Plugin::setInfo(PluginInfo info)
{
    info_ = std::move(info);
}

String Plugin::name() const
{
    return info_.name;
}

std::vector<CommandInfo> Plugin::commandInfos() const
{
    auto* app = Application::current();
    auto* cm  = app ? app->commandManager() : nullptr;
    return cm ? cm->commandInfosForPlugin(name()) : std::vector<CommandInfo>{};
}

std::vector<const ConfigItem*> Plugin::configItems() const
{
    auto* app = Application::current();
    auto* reg = app ? app->configRegistry() : nullptr;
    return reg ? reg->itemsForPlugin(name()) : std::vector<const ConfigItem*>{};
}

vn::async::Task<void> Plugin::preLoad(PluginLoadContext* context)
{
    // Commands (VN_DECLARE_COMMAND) are registered by the PluginManager through
    // the plugin DLL's vinePluginRegisterCommands entry, which runs inside the
    // plugin module and flushes its per-module queue.
    (void)context;
    // Every hook body is a coroutine, `co_return` included: a body without it is an ordinary function that returns an
    // empty task, and the caller waits for work that never runs (see the note on Plugin::load()).
    co_return;
}

vn::async::Task<void> Plugin::load(PluginLoadContext* context)
{
    (void)context;
    co_return;
}

vn::async::Task<void> Plugin::postLoad(PluginLoadContext* context)
{
    (void)context;
    co_return;
}

void Plugin::unload(PluginLoadContext* context)
{}
VN_APPFW_NS_END
