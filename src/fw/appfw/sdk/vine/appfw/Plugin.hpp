#pragma once
#include "appfw_global.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <vine/Object.hpp>
#include <vine/Uuid.hpp>
#include <vine/async/Task.hpp>

/**
 * @brief ABI revision of the plugin-facing SDK.
 *
 * Bump it whenever anything a plugin compiles against changes incompatibly:
 * PluginAbi and this header, PluginInfo, Plugin, PluginLoadContext, the entry-point
 * signatures in plugin_export.hpp, the command-registration ABI, or the signature of
 * a virtual in an exported SDK class (2u: UserIO::getIntAsync now yields an int
 * instead of an int8_t). The host refuses a plugin whose library reports another
 * revision (see PluginAbi), so a bump means "the plugins must be rebuilt" - which
 * is what such a change requires anyway.
 *
 * 3u: ProgressHost moved from the base progress module into appfw, so plugs that
 * reported progress through vn::progress::ProgressHost include it from here now.
 *
 * 4u: Application::init() was folded into the constructor. The virtual was removed
 * from the middle of Application's vtable, so every slot after it moved - a plugin
 * built against 3u would call the wrong ones. AppConfig/SplashConfig also moved to
 * their own header (AppBuilder.hpp includes it, so existing includes still work).
 *
 * 5u: the boot's three phases became async methods (Application::startupStart(),
 * startup() and startupEnd() return vn::async::Task<void> instead of taking a
 * continuation) and are declared as one set at the end of the class, so
 * Application's vtable changed shape once more - a plugin built against 4u would
 * call the wrong slots.
 *
 * 6u: the plugin lifecycle's three hooks (preLoad(), load() and postLoad()) return
 * vn::async::Task<void> instead of void. A plugin's startup work happens on the
 * application thread (widgets, graphics objects, render sessions), so the only way an
 * application thread stretch stops blocking the loop is for the plugin itself to send
 * its UI-free half to the pool and come back - and that needs an await. The manager
 * does not turn the loop; it reports per phase instead (see PluginManager). unload()
 * deliberately stays synchronous: it runs while the loop is already down.
 *
 * 7u: the SDK's text and its cross-thread helpers were unified. The text a plugin hands
 * to the framework is vn::String, not std::string: StartupProgress::stage()/setLabel()/
 * label(), ProgressHost::setLabel()/label()/scope() and the AppConfig/SplashConfig
 * fields. StartupProgress::advance() became setDone() (the count was always absolute,
 * the name said otherwise) and its isCounted()/fraction() pair became one
 * std::optional<double> fraction() - there is no longer a flag a caller can pair with
 * the wrong number. The MainThreadDispatcher helpers a worker can reach without an
 * object are static now (isMainThread(), hasEventLoop(), postToMainThread(),
 * invokeOnMainThread(), resumeOnMainThread()) and the instance-only postToMain() is
 * gone. Application::startupProgress() is gone too: StartupProgress::current() is the
 * one way to reach the boot's sink.
 *
 * The name says *plugin* ABI on purpose: it is not the release version (that is
 * VN_APPFW_VERSION in appfw_global.hpp, diagnostic only), and it says nothing about
 * the host's own binaries, which are built and rebuilt together with the framework.
 */
#define VN_APPFW_PLUGIN_ABI_VERSION 7u

VN_APPFW_NS_BEGIN

class PluginLoadContext;
class ConfigItem;
struct CommandInfo;

/**
 * @brief ABI handshake a plugin library answers before the host reads its metadata.
 *
 * The host has to know *how* a plugin was built before it interprets anything the
 * plugin declares. PluginInfo is a struct whose layout follows this SDK, so reading
 * it from a library built against another SDK silently misinterprets its fields - a
 * String read at the wrong offset is a length and a pointer of garbage, and the
 * failure surfaces later as a crash or as nonsense in the UI.
 *
 * This struct is the small, deliberately stable prefix that is read first. Rules:
 *
 * - abi_version stays the first member, and the host reads no other member until it
 *   matches its own VN_APPFW_PLUGIN_ABI_VERSION (the static_asserts below keep it first);
 * - members may only be appended, never reordered or removed, and they must not use
 *   SDK types whose own layout can change (plain integers and const char* only);
 * - VN_APPFW_PLUGIN_ABI_VERSION is bumped by any change to that surface (its own
 *   documentation lists what counts), which is what turns "silently misread
 *   metadata" into one clear refusal.
 */
struct VN_APPFW_API PluginAbi {
    std::uint32_t abi_version{ 0 };      ///< VN_APPFW_PLUGIN_ABI_VERSION the library was compiled with.
    const char*   framework_version{};   ///< VN_APPFW_VERSION it was built against; never null, UTF-8.
};

static_assert(std::is_standard_layout_v<PluginAbi>, "PluginAbi is read across a module boundary and must stay standard layout");
static_assert(offsetof(PluginAbi, abi_version) == 0, "abi_version must stay the first member: the host reads it before the layout is trusted");

/**
 * @brief Static metadata declared by a plugin.
 *
 * The layout is part of the plugin ABI: a field added or reordered here is a change
 * every plugin must be rebuilt for, so VN_APPFW_PLUGIN_ABI_VERSION is bumped together
 * with it.
 */
struct VN_APPFW_API PluginInfo {
    ///
    /// Stable plugin identity, hardcoded by the plugin (VN_DECLARE_PLUGIN) and
    /// independent of its name and of where its library is installed. The null
    /// UUID means "not declared": identity then falls back to name.
    Uuid                uuid;
    String              name;         // Unique plugin name (identifier).
    String              display_name; // Human-friendly name shown in UI; falls back to name when empty.
    String              version;      // Plugin version, e.g. "1.2.0".
    String              description;  // Human-readable description.
    String              vendor;       // Author or vendor.
    String              email;        // Contact address of the vendor; optional.
    String              repo;         // Source repository or issue tracker URL; optional.
    ///
    /// Inline SVG source used as the plugin's icon in the UI. Empty means "no
    /// icon": the plugin manager then draws its own default one. Keep it to plain
    /// shapes - the host renders it with QSvgRenderer, which supports a static SVG
    /// subset - and a source that cannot be parsed falls back to the default icon
    /// as well.
    String              icon;
    std::vector<String> dependencies; // Plugin names required before this one.
};

/**
 * @brief Base class of a loadable plugin.
 *
 * A plugin is an Object loaded by the PluginManager. The lifecycle is: preLoad()
 * for every plugin, then load() for every plugin, then postLoad() for every
 * plugin once all have loaded (cross-plugin wiring); on shutdown unload() runs
 * for every loaded plugin in reverse dependency order (dependents first).
 *
 * A throw out of preLoad(), load() or postLoad() is caught by the manager, which
 * unloads the instance again (best effort) and reports the load as failed: a
 * plugin library can be created only once per process, so unload() must tolerate
 * being called on a plugin whose load() did not complete.
 */
class VN_APPFW_API Plugin : public Object {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Returns the plugin's static metadata.
     *
     * Populated by the PluginManager from the plugin's vinePluginQuery() entry
     * when the plugin is created; VN_DECLARE_PLUGIN is the single source of the
     * metadata.
     *
     * @return The plugin metadata.
     */
    const PluginInfo& info() const;

    /**
     * @brief Returns the plugin name (convenience for info().name).
     *
     * @return The plugin name.
     */
    String name() const;

    /**
     * @brief Reports the commands registered by this plugin.
     *
     * Queries the host CommandManager for commands attributed to this plugin
     * (see CommandManager::setRegistrationOwner).
     *
     * @return The plugin's command metadata (may be empty).
     */
    std::vector<CommandInfo> commandInfos() const;

    /**
     * @brief Reports the config items registered by this plugin.
     *
     * Queries the host ConfigRegistry for items owned by this plugin.
     *
     * @return The plugin's config items (may be empty).
     */
    std::vector<const ConfigItem*> configItems() const;

  public:
    /**
     * @brief Called before load(); used for dependency checks and early setup.
     *
     * A coroutine, like the two hooks after it: see load() for what the await buys.
     *
     * @param context Load context exposing host capabilities.
     * @return A task that completes when this phase is done on the application thread.
     */
    virtual vn::async::Task<void> preLoad(PluginLoadContext* context);

    /**
     * @brief Registers the plugin's contributions (services, configs, ...).
     *
     * Runs on the application thread and returns once the plugin is usable. Its work is SPLIT, not moved: a plugin that
     * has something expensive and UI-free to do sends it to the pool - `co_await vn::async::run(...)`, then
     * `co_await MainThreadDispatcher::resumeOnMainThread()` before touching UI again - while the widgets and
     * graphics objects it builds stay on this thread, as they must.
     *
     * What this buys the boot: while the hook is suspended on the pool the event loop runs freely, so a progress report
     * made there is delivered, and the interface stays alive. What it does NOT do is keep a long application-thread
     * stretch animated - nothing can paint that window while its thread is inside that stretch - so keep those stretches
     * short, and prefer the pool for anything that does not need a widget or a graphics object.
     *
     * @note A hook that does only synchronous work is still a coroutine and must end with `co_return;`: without a
     *       `co_await`/`co_return` the body compiles as an ordinary function returning an empty task, and the call site
     *       waits for something that never runs (the framework's own driver hit this as a `ud2` at run time).
     *
     * @param context Load context exposing host capabilities.
     * @return A task that completes when the plugin is usable.
     */
    virtual vn::async::Task<void> load(PluginLoadContext* context);

    /**
     * @brief Called after every plugin has loaded; used for cross-plugin wiring.
     *
     * A coroutine, like the two hooks before it: see load() for what the await buys.
     *
     * @param context Load context exposing host capabilities.
     * @return A task that completes when this phase is done on the application thread.
     */
    virtual vn::async::Task<void> postLoad(PluginLoadContext* context);

    /**
     * @brief Releases the plugin's resources on shutdown.
     *
     * Called by the PluginManager for every loaded plugin, in reverse dependency
     * order (a plugin is unloaded before the plugins it depends on), after the
     * main loop has stopped but while the application, the command manager, the
     * event bus and the UI are still alive, so the plugin can still publish on
     * the bus or remove its own subscriptions.
     *
     * A disabled plugin was never loaded and is therefore never unloaded. The host
     * does not revoke the plugin's registrations before calling this (its managers
     * are destroyed right afterwards), so unload() releases plugin-owned resources
     * (workers, threads, sinks, subscriptions), not host registrations. Throwing
     * from unload() is caught and logged; the remaining plugins are still
     * unloaded.
     *
     * unload() is also called after a failed load() (see the class comment), so it
     * must not assume that preLoad()/load()/postLoad() all ran; an implementation
     * that registers resources in load() must release them defensively here.
     *
     * @param context Load context exposing host capabilities.
     */
    virtual void unload(PluginLoadContext* context);

  private:
    friend class PluginManager;

    /**
     * @brief Sets the plugin's static metadata.
     *
     * Called by the PluginManager when the plugin is created (from the plugin's
     * vinePluginQuery() entry); plugin authors do not call this.
     *
     * @param info Plugin metadata.
     */
    void setInfo(PluginInfo info);

    /// Static metadata set by the PluginManager at load time.
    PluginInfo info_;
};

VN_APPFW_NS_END
