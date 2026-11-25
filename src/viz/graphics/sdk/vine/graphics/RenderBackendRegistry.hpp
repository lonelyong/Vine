#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include <vine/String.hpp>
#include <vine/raw_ptr.hpp>

#include "BackendAbi.hpp"
#include "Diagnostics.hpp"
#include "RenderBackend.hpp"

VN_GRAPHICS_NS_BEGIN

class RenderBackend;
class Scene;
class Camera;

/**
 * @brief Graphics API families a render backend can use.
 *
 * @details
 * - A backend may support several families simultaneously (e.g. OGRE renders through OpenGL, Direct3D or Vulkan at
 *   runtime), so the values are bit flags: combine with `|` and test with vn::testFlag().
 */
enum class RenderApi : std::uint32_t
{
    None     = 0,
    Vulkan   = 1 << 0, // Vulkan.
    OpenGL2  = 1 << 1, // OpenGL 2.x (fixed-function pipeline).
    OpenGL3  = 1 << 2, // OpenGL 3.x+ (core profile).
    OpenGLES = 1 << 3, // OpenGL ES (mobile/embedded).
    Direct3D = 1 << 4, // Direct3D 11/12.
};

VN_ENABLE_ENUM_FLAGS(RenderApi)

/** @brief Converts render API flags to a combined display string.
 *
 * @details
 * - PUBLIC ON PURPOSE: a host names the API it picked in its own UI (the app shell's backend list prints these
 *   flags), so this is host vocabulary rather than registry bookkeeping (`G4`: a name the host can call, saying
 *   exactly what it returns).
 * - The spelling is one word per flag in declaration order, so two backends that answer the same flags print the
 *   same line.
 *
 * @param api Render API flags.
 * @return Lower-case list joined with " | ", e.g. "vulkan | opengl3"; "unknown" when no flag is set.
 */
VN_GRAPHICS_API String renderApiToString(RenderApi api);

/**
 * @brief Static metadata describing a render backend implementation.
 *
 * @details
 * - One backend plugin may register several factories (e.g. different GL versions); each factory carries its own
 *   RenderBackendInfo.
 */
struct VN_GRAPHICS_API RenderBackendInfo {
    String    name;                        // Unique backend name (identifier) - the key create() matches.
    String    display_name;                // Human-friendly name shown in UI; falls back to name when empty.
    String    description;                 // Human-readable description.
    String    version;                     // Backend version, e.g. "1.0.0".
    String    vendor;                      // Author or vendor.
    RenderApi api_flags = RenderApi::None; // Graphics API families used by the backend (bit flags).
};

/**
 * @brief Factory creating a concrete render backend.
 *
 * @details
 * - A concrete backend implements this interface and self-registers into
 *   RenderBackendRegistry, so the application can create a backend by name without a compile-time dependency on the
 *   backend module or its third-party libraries.
 * - A single backend plugin can register several factories, one per implementation.
 * - THIS VTABLE IS WHAT THE ABI REVISION PROTECTS: a factory is the first object a host touches across a module
 *   boundary, so the shape it was compiled with is `VN_RENDER_BACKEND_ABI_VERSION` (see BackendAbi).
 *   The module states that revision through `vineRenderBackendAbi()`, and the registry checks it before calling
 *   `info()` or `name()` on anything - which is also why the revision cannot be a member of `info()`.
 */
class VN_GRAPHICS_API RenderBackendFactory {
  public:
    virtual ~RenderBackendFactory() = default;

    /** @brief Gets the backend's static metadata.
     *
     * @return The backend metadata.
     */
    virtual RenderBackendInfo info() const = 0;

    /** @brief Gets the backend name (convenience for info().name).
     *
     * @return The backend name.
     */
    virtual String name() const
    {
        return info().name;
    }

    /** @brief Creates a backend instance, told how its device presents.
     *
     * @details
     * - The engine owns the pipeline and drives content per pass, so a backend is not bound to a Vine scene or
     *   camera: its content slots are created lazily from the draw packets a frame's value carries, keyed by the
     *   pass identity its entry names (see FrameDescription::Pass).
     * - THE CREATION INPUT is @p desc (see DeviceDesc): `Probe` (the default) is a device that brings up a window of
     *   its own, `None` one that presents nothing at all (created without a window), and `Handle` one made for the
     *   host's window.
     *
     * @param desc How the device reaches the display (see DeviceDesc).
     * @return Newly created backend; the caller owns the returned reference.
     */
    virtual vn::intrusive_ptr<RenderBackend> create(const DeviceDesc& desc = {}) = 0;
};

/**
 * @brief Why a backend registration did or did not take (see RenderBackendRegistry::registerFactory).
 *
 * @details
 * - THE MACHINE ANSWER, so a host or a test gates on it instead of matching a sentence: the sentence travels on the
 *   registry's own diagnostic route (see RenderBackendRegistry::diagnostics), which is the same split the device's
 *   createSurfaceTarget uses - a reason out, the words on the channel.
 * - The refusals are not collapsed into one "failed": each says what has to be rebuilt or rewired, so a host can act
 *   on it without reading the message.
 */
enum class BackendRegistrationResult : std::uint8_t
{
    Ok,             // The factory now owns its name.
    AbiMissing,     // The module stated no SDK revision: it was built before the rule existed - rebuild it.
    AbiMismatch,    // The module speaks another SDK revision: rebuild it against this one.
    InvalidFactory, // A null factory, a wiring defect at the caller - reported nowhere.
};

/**
 * @brief Registry of render backend factories.
 *
 * @details
 * - Backend modules self-register a RenderBackendFactory here (typically via a static Registrar object in the
 *   backend's translation unit).
 * - The application then creates a backend by name without depending on the backend module at compile time.
 * - A REGISTRATION IS A CROSS-MODULE HAND-OVER, so it is gated: the registering module states the graphics SDK ABI it
 *   was compiled against (RenderBackendAbi) and this registry refuses a module built against another revision before
 *   it calls anything that module declares.
 *   The rule is the SDK's, and its reasons are in `VN_RENDER_BACKEND_ABI_VERSION`; the registry is where it is
 *   enforced because that is the one place every backend passes through.
 * - A LOAD-TIME EVENT HAS NO ENGINE YET (a static Registrar runs while the process starts, a plugin registers while
 *   it loads), so the registry owns the diagnostic route those refusals travel on (see diagnostics).
 */
class VN_GRAPHICS_API RenderBackendRegistry {
  public:
    /** @brief Read-only snapshot of a registered backend factory. */
    struct Entry {
        RenderBackendInfo             info;    // Backend metadata (name, description, ...).
        raw_ptr<RenderBackendFactory> factory; // Registered factory (not owned by the registry).
    };

    /** @brief Gets the process-wide registry singleton. */
    static RenderBackendRegistry& instance();

    /** @brief Registers a factory, accepting it only when the module was built against this SDK's ABI revision.
     *
     * @details
     * - THE CHECK COMES FIRST, and that is the whole reason @p abi is a parameter: a module built against another
     *   revision has a different `RenderBackendFactory` vtable and a different `RenderBackendInfo` layout, so asking
     *   it for `info()` or `name()` - even to write a better message - is the call that crashes.
     *   The registry compares @p abi, and only a compatible module is ever asked anything.
     * - The registry does not own the factory; it must outlive the registry.
     * - STILL LAST-ONE-WINS BETWEEN VALID MODULES: two modules that both speak this revision replace each other in
     *   registration order, and the replacement is not reported.
     *   WHICH POLICY SHOULD REPLACE THAT IS NOT DECIDED HERE: backlog `T64` records the invariant any policy must keep
     *   (for a given configuration the winner follows the module identity, never the load order) and leaves the policy
     *   itself to the host as a configuration option, deferred until a host needs the choice.
     * - A REFUSAL IS COUNTED AND REPORTED on diagnostics(), with one of two stable codes
     *   (`engine.backend.abi_missing`, `engine.backend.abi_mismatch`) and a sentence naming both revisions.
     *   The report carries no subject on purpose: naming the backend means calling the module, which is exactly what
     *   the refusal avoids.
     *
     * @param factory Factory to register; a null one is refused without a report (`InvalidFactory`).
     * @param abi     The module's handshake, i.e. `vineRenderBackendAbi()` from the module being registered.
     * @return Ok when the factory now owns its name; otherwise why it was refused (see BackendRegistrationResult).
     */
    [[nodiscard]] BackendRegistrationResult registerFactory(raw_ptr<RenderBackendFactory> factory, const RenderBackendAbi* abi);

    /** @brief Registers a factory that states no SDK revision: REFUSED, and reported.
     *
     * @details
     * - THE COMPATIBILITY SHIM, AND ITS ONE JOB: a module compiled before `VN_RENDER_BACKEND_ABI_VERSION` existed calls
     *   this overload, so the call RESOLVES and is refused with a sentence instead of failing as a missing symbol
     *   inside that module's own load (which aborts the process on Linux, with no message a host can act on).
     *   It is not a way to register.
     * - It stays until no module built before the rule can be in the wild; the next revision bump is the natural point
     *   to delete it, because a module that old is then refused by its revision rather than by its signature.
     * - Same rule as its sibling: nothing is asked of @p factory, so a stale module is never called into.
     *
     * @param factory Factory to register; not read (the module cannot say what it was built against).
     * @return AbiMissing, always.
     */
    [[nodiscard]] BackendRegistrationResult registerFactory(raw_ptr<RenderBackendFactory> factory);

    /** @brief Creates a backend by name, told how its device presents.
     *
     * @param name Backend name - the key a factory registered under (see RenderBackendInfo::name).
     * @param desc How the device reaches the display (see DeviceDesc).
     * @return New backend, or null when no factory with that name is registered.
     */
    vn::intrusive_ptr<RenderBackend> create(const String& name, const DeviceDesc& desc = {}) const;

    /** @brief Gets the names of all registered backends. */
    std::vector<String> names() const;

    /** @brief Gets a snapshot of all registered backends.
     *
     * @details
     * - Safe to call concurrently; the returned snapshot is independent of the registry's internal state. Use it to
     *   iterate and query the registered backends, e.g. to list available backends or inspect their factories.
     *
     * @return One entry per registered backend, ordered by name.
     */
    std::vector<Entry> entries() const;

    /** @brief Gets the registry's own diagnostic route: where a refused registration is reported.
     *
     * @details
     * - A LOAD-TIME EVENT HAS NO ENGINE YET: a backend registers while the process is starting (a static Registrar)
     *   or while a plugin loads, and neither has an engine to hand a report to.
     *   The registry therefore owns the one route those reports travel on, and a host that wants the sentence installs
     *   its sink here BEFORE it loads backend modules (a sink installed later sees the counts, not the words).
     * - ONE ROUTE, ONE PLACE TO LISTEN (see Diagnostics): the same instance counts and forwards, so a host gates on
     *   `total()` rather than parsing a log.
     *
     * @return The route, borrowed; it lives as long as the process, because the registry is a singleton.
     */
    Diagnostics& diagnostics() noexcept;

    /** @brief Whether a backend with the given name is registered. */
    bool has(const String& name) const;

    /** @brief RAII helper registering a factory on construction.
     *
     * @details
     * - It states the ABI through the handshake rather than taking one: the module that instantiates a Registrar must
     *   define `vineRenderBackendAbi()` once with VN_DECLARE_RENDER_BACKEND_ABI(), so the revision checked is the one
     *   that module was compiled against, not the one this header happens to be parsed with.
     * - A refusal is reported on the registry's route and NOT thrown or asserted: a static registration runs during
     *   startup, where failing loudly would take the whole process down instead of leaving one backend unavailable.
     *
     * @note Instantiate as a static object in the backend module to self-register.
     */
    template <typename TFactory>
    class Registrar {
      public:
        Registrar()
        {
            (void)RenderBackendRegistry::instance().registerFactory(&factory_, ::vineRenderBackendAbi());
        }

      private:
        TFactory factory_;
    };

  private:
    RenderBackendRegistry();
    ~RenderBackendRegistry();
    RenderBackendRegistry(const RenderBackendRegistry&)            = delete;
    RenderBackendRegistry& operator=(const RenderBackendRegistry&) = delete;

    struct Data;
    // Owns the registry state through RAII (see the repo's "avoid raw owning
    // pointers" rule); the out-of-line destructor keeps Data incomplete here.
    std::unique_ptr<Data> d;
};

VN_GRAPHICS_NS_END
