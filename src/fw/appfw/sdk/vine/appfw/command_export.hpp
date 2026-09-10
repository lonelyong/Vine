#pragma once

#include "CommandManager.hpp"

#include <functional>
#include <utility>
#include <vector>

/**
 * @brief Binds a symbol to the module that defines it.
 *
 * A symbol that is not exported stays private to its module: the dynamic linker
 * neither merges nor interposes it, so each plugin library keeps its own copy of
 * what it defines. That is what makes the command queue below the module's own.
 * On Windows anything without __declspec(dllexport) is already module-private, so
 * the attribute is only needed on ELF platforms.
 */
#if defined(V_CC_MSVC)
#    define V_MODULE_LOCAL
#else
#    define V_MODULE_LOCAL __attribute__((visibility("hidden")))
#endif

V_APPFW_NS_BEGIN
namespace detail {

/**
 * @brief Registrar callback: registers one command with a CommandManager.
 */
using CommandRegistrar = std::function<bool(CommandManager*)>;

/**
 * @brief Returns the command registrars declared in the calling module.
 *
 * Defined once per module by V_DEFINE_MODULE_COMMAND_QUEUE(), which
 * V_DECLARE_PLUGIN() expands for a plugin library, and deliberately neither an
 * inline function nor a variable of one: an inline function with a function-local
 * static is *one object for the whole process*, even across dlopen-ed libraries,
 * because the compiler gives that static the GNU unique binding (and the inline
 * function itself a weak, preemptible symbol). Every plugin would then queue its
 * commands into the same container, and the first plugin whose registration entry
 * ran would flush the commands of every plugin that had merely been *discovered* -
 * including the ones the user disabled or the host skipped, which would then
 * become runnable and be attributed to the wrong plugin.
 *
 * V_MODULE_LOCAL is what makes the queue the module's own: V_DECLARE_COMMAND()
 * queues into the calling module's container, and the module's own registration
 * entry point (vinePluginRegisterCommands) flushes exactly that container.
 *
 * @return The module-local queue of pending command registrars.
 */
V_MODULE_LOCAL std::vector<CommandRegistrar>& moduleCommandQueue();

/**
 * @brief Registers the queued commands of one module and empties its queue.
 *
 * Takes the queue explicitly, so the whole flush is a function of its arguments:
 * the caller is the module's own registration entry point and passes its own
 * container (see moduleCommandQueue()), which means nothing here can be interposed by
 * another library.
 *
 * @param registrars Module-local queue of command registrars.
 * @param manager    Command manager to register into; nullptr is ignored.
 */
inline void flushQueuedCommands(std::vector<CommandRegistrar>& registrars, CommandManager* manager)
{
    if (!manager) {
        return;
    }
    // Moved out before running: a registrar may load another plugin (or otherwise
    // queue further commands), and those must not be flushed by this call as well.
    std::vector<CommandRegistrar> batch = std::move(registrars);
    registrars.clear();
    for (auto& registrar : batch) {
        registrar(manager);
    }
}

} // namespace detail
V_APPFW_NS_END

/**
 * @brief Defines the module-local command queue of the module that uses it.
 *
 * Must appear exactly once per module that declares commands with
 * V_DECLARE_COMMAND(); V_DECLARE_PLUGIN() expands it for a plugin library. A module
 * that declares commands without defining the queue fails to link, which is the
 * intended outcome: the queue must belong to exactly one module.
 *
 * The definition is spelled with a qualified name so the macro works both inside
 * the appfw namespace and at global scope, which is where a plugin entry point may
 * live.
 */
#define V_DEFINE_MODULE_COMMAND_QUEUE()                                                  \
    V_MODULE_LOCAL std::vector<vine::appfw::detail::CommandRegistrar>&                    \
    vine::appfw::detail::moduleCommandQueue()                                             \
    {                                                                                    \
        static std::vector<vine::appfw::detail::CommandRegistrar> registrars;             \
        return registrars;                                                               \
    }

/**
 * @brief Declares a command inside its class body.
 *
 * Overrides the virtual name() with a compile-time constant and queues the
 * command with this module's registration queue (see V_DEFINE_MODULE_COMMAND_QUEUE()).
 * The command is registered with the CommandManager while the plugin is loaded: the
 * PluginManager calls the plugin's exported vinePluginRegisterCommands entry (see
 * plugin_export.hpp), which flushes this module's queue - never during module (DLL)
 * load. Because the registrar is an inline static member, a command declared in a
 * header is queued exactly once per module, and because the queue belongs to the
 * module, no other plugin can flush it.
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
            vine::appfw::detail::moduleCommandQueue().push_back(                 \
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
            vine::appfw::detail::moduleCommandQueue().push_back(                 \
                [](vine::appfw::CommandManager* manager) {                       \
                    return manager->registerAlias(AliasName, TargetName);        \
                });                                                              \
        }                                                                        \
    };                                                                           \
    inline static AutoAliasRegistrar s_auto_alias_registrar_{};
