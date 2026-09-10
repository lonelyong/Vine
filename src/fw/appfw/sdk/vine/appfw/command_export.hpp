#pragma once

#include "CommandManager.hpp"

#include <functional>
#include <vector>

V_APPFW_NS_BEGIN
namespace detail {

/**
 * @brief Registrar callback: registers one command with a CommandManager.
 */
using CommandRegistrar = std::function<bool(CommandManager*)>;

/**
 * @brief Returns the command registrars declared in the current module.
 *
 * Defined inline in a header, so every module that includes it gets its own
 * copy of the queue: each plugin DLL accumulates only the commands declared by
 * that plugin. The queue is flushed when the module's registration entry runs:
 * the PluginManager resolves the plugin DLL's vinePluginRegisterCommands (exported
 * by V_DECLARE_PLUGIN) and calls it while loading the plugin, so registration
 * happens during plugin loading - never during module (DLL) load, and never from
 * Plugin::preLoad(), which is empty by design.
 *
 * @return The module-local queue of pending command registrars.
 */
inline std::vector<CommandRegistrar>& moduleCommandRegistrars()
{
    static std::vector<CommandRegistrar> registrars;
    return registrars;
}

/**
 * @brief Registers every command declared in the current module and clears the
 * queue.
 *
 * @param manager Command manager to register into; nullptr is ignored.
 */
inline void registerModuleCommands(CommandManager* manager)
{
    if (!manager) {
        return;
    }
    auto& registrars = moduleCommandRegistrars();
    for (auto& registrar : registrars) {
        registrar(manager);
    }
    registrars.clear();
}

} // namespace detail
V_APPFW_NS_END

/**
 * @brief Declares a command inside its class body.
 *
 * Overrides the virtual name() with a compile-time constant and queues the
 * command with this module's registration queue. The command is registered with
 * the CommandManager while the plugin is loaded: the PluginManager calls the
 * plugin's exported vinePluginRegisterCommands entry (see plugin_export.hpp),
 * which flushes this queue - never during module (DLL) load. Because the
 * registrar is an inline static member, a command declared in a header is
 * queued exactly once per module.
 *
 * Place inside the command class:
 * @code
 * class MyCommand : public vine::appfw::Command {
 *     V_OBJECT_META_DECL;
 *     V_DECLARE_COMMAND(MyCommand, u8"myCommand")
 *   public:
 *     vine::appfw::String group() const override { return u8"Edit"; }
 *     vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::None; }
 *     vine::appfw::CommandResult execute(vine::appfw::CommandExecutionContext*) override;
 * };
 * @endcode
 *
 * @param CommandClass The command class.
 * @param CommandName The command name as a u8"..." literal.
 */
#define V_DECLARE_COMMAND(CommandClass, CommandName)                             \
  public:                                                                        \
    vine::String name() const override { return CommandName; }                   \
  private:                                                                       \
    struct AutoRegistrar {                                                       \
        AutoRegistrar()                                                          \
        {                                                                        \
            vine::appfw::detail::moduleCommandRegistrars().push_back(            \
                [](vine::appfw::CommandManager* manager) {                       \
                    return manager->registerCommand<CommandClass>(CommandName);  \
                });                                                              \
        }                                                                        \
    };                                                                           \
    inline static AutoRegistrar s_auto_registrar_{};

/**
 * @brief Declares an alias for a command name inside a command class body.
 *
 * Queues an alias registration with this module's command registration queue;
 * the alias resolves to the target command name when commands execute by name.
 * Place inside the command class that owns the alias.
 *
 * @code
 * class ListCommandsCommand : public vine::appfw::Command {
 *     V_OBJECT_META_DECL;
 *     V_DECLARE_COMMAND(ListCommandsCommand, u8"list_commands")
 *     V_DECLARE_COMMAND_ALIAS(u8"gcm", u8"list_commands")
 *   public:
 *     ...
 * };
 * @endcode
 *
 * @param AliasName The alias as a u8"..." literal.
 * @param TargetName The canonical command name the alias resolves to.
 */
#define V_DECLARE_COMMAND_ALIAS(AliasName, TargetName)                            \
  private:                                                                       \
    struct AutoAliasRegistrar {                                                  \
        AutoAliasRegistrar()                                                     \
        {                                                                        \
            vine::appfw::detail::moduleCommandRegistrars().push_back(            \
                [](vine::appfw::CommandManager* manager) {                       \
                    return manager->registerAlias(AliasName, TargetName);        \
                });                                                              \
        }                                                                        \
    };                                                                           \
    inline static AutoAliasRegistrar s_auto_alias_registrar_{};
