#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include <vine/String.hpp>
#include <vine/raw_ptr.hpp>

#include "RenderBackend.hpp"

V_GRAPHICS_NS_BEGIN

class RenderBackend;
class Scene;
class Camera;

/**
 * @brief Graphics API families a render backend can use.
 *
 * A backend may support several families simultaneously (e.g. OGRE renders
 * through OpenGL, Direct3D or Vulkan at runtime), so the values are bit
 * flags: combine with `|` and test with vine::testFlag().
 */
enum class RenderApi : std::uint32_t {
    None     = 0,
    Vulkan   = 1 << 0,  // Vulkan.
    OpenGL2  = 1 << 1,  // OpenGL 2.x (fixed-function pipeline).
    OpenGL3  = 1 << 2,  // OpenGL 3.x+ (core profile).
    OpenGLES = 1 << 3,  // OpenGL ES (mobile/embedded).
    Direct3D = 1 << 4,  // Direct3D 11/12.
};

V_ENABLE_ENUM_FLAGS(RenderApi)

/** @brief Converts render API flags to a combined display string.
 *
 * @param api Render API flags.
 * @return Lower-case list joined with " | ", e.g. "vulkan | opengl3";
 *         "unknown" when no flag is set.
 */
V_GRAPHICS_API String renderApiToString(RenderApi api);

/**
 * @brief Static metadata describing a render backend implementation.
 *
 * One backend plugin may register several factories (e.g. different GL
 * versions); each factory carries its own RenderBackendInfo.
 */
struct V_GRAPHICS_API RenderBackendInfo {
    String name;          // Unique backend name (identifier), e.g. "vsg", "opengl3".
    String display_name;  // Human-friendly name shown in UI; falls back to name when empty.
    String description;   // Human-readable description.
    String version;       // Backend version, e.g. "1.0.0".
    String vendor;        // Author or vendor.
    RenderApi api_flags = RenderApi::None;  // Graphics API families used by the backend (bit flags).
};

/**
 * @brief Factory creating a concrete render backend.
 *
 * Concrete backends (vsg, hand-written Vulkan, OpenGL, ...) implement this
 * interface and self-register into RenderBackendRegistry, so the application
 * can create a backend by name without a compile-time dependency on the
 * backend module or its third-party libraries. A single backend plugin can
 * register several factories, one per implementation.
 */
class V_GRAPHICS_API RenderBackendFactory {
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
    virtual String name() const { return info().name; }

    /** @brief Creates a backend instance.
     *
     * The engine owns the pipeline and drives content per pass, so a backend
     * is not bound to a Vine scene or camera: window layers are created
     * lazily from the per-pass render() calls the engine drives.
     *
     * @return Newly created backend; the caller owns the returned reference.
     */
    virtual vine::intrusive_ptr<RenderBackend> create() = 0;
};

/**
 * @brief Registry of render backend factories.
 *
 * Backend modules self-register a RenderBackendFactory here (typically via a
 * static Registrar object in the backend's translation unit). The application
 * then creates a backend by name without depending on the backend module at
 * compile time.
 */
class V_GRAPHICS_API RenderBackendRegistry {
  public:
    /** @brief Read-only snapshot of a registered backend factory. */
    struct Entry {
        RenderBackendInfo info;        // Backend metadata (name, description, ...).
        raw_ptr<RenderBackendFactory> factory;  // Registered factory (not owned by the registry).
    };

    /** @brief Gets the process-wide registry singleton. */
    static RenderBackendRegistry& instance();

    /** @brief Registers a factory.
     *
     * The registry does not own the factory; it must outlive the registry.
     *
     * @param factory Factory to register.
     */
    void registerFactory(raw_ptr<RenderBackendFactory> factory);

    /** @brief Creates a backend by name.
     *
     * @param name Backend name, e.g. "vsg".
     * @return New backend, or null when no factory with that name is
     *         registered.
     */
    vine::intrusive_ptr<RenderBackend> create(const String& name) const;

    /** @brief Gets the names of all registered backends. */
    std::vector<String> names() const;

    /** @brief Gets a snapshot of all registered backends.
     *
     * Safe to call concurrently; the returned snapshot is independent of the
     * registry's internal state. Use it to iterate and query the registered
     * backends, e.g. to list available backends or inspect their factories.
     *
     * @return One entry per registered backend, ordered by name.
     */
    std::vector<Entry> entries() const;

    /** @brief Whether a backend with the given name is registered. */
    bool has(const String& name) const;

    /** @brief RAII helper registering a factory on construction.
     *
     * Instantiate as a static object in the backend module to self-register.
     */
    template <typename TFactory>
    class Registrar {
      public:
        Registrar() { RenderBackendRegistry::instance().registerFactory(&factory_); }

      private:
        TFactory factory_;
    };

  private:
    RenderBackendRegistry();
    ~RenderBackendRegistry();
    RenderBackendRegistry(const RenderBackendRegistry&) = delete;
    RenderBackendRegistry& operator=(const RenderBackendRegistry&) = delete;

    struct Data;
    // Owns the registry state through RAII (see the repo's "avoid raw owning
    // pointers" rule); the out-of-line destructor keeps Data incomplete here.
    std::unique_ptr<Data> d;
};

V_GRAPHICS_NS_END
