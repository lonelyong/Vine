#include <gtest/gtest.h>

#include <QStandardPaths>

#include <vine/appfw/AppBuilder.hpp>
#include <vine/appfw/Application.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/Plugin.hpp>
#include <vine/vsg/api/DeviceProbe.hpp>
#include <vine/vsg/api/VsgBackend.hpp>

#include <vine/Buffer.hpp>

// TestHostWindow is an XCB window, so the case that uses it reaches for the connection itself; the guard
// matches the one around that case.
#if !defined(_WIN32)
#    include <xcb/xcb.h>
#endif

#include "TestHostWindow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include <memory>
#include <vector>

using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::RenderCommand;
using vn::graphics::RenderPass;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;

namespace
{

/**
 * @brief Boots an Application the same way app/src/main.cpp does, but headless.
 *
 * Mirrors the real startup: build the application through the builder, then
 * load all plugins. PluginManager discovers gfx_backend_vsg from the default
 * plugin directory; GfxBackendVsgPlugin::load() registers the "vsg" backend
 * into RenderBackendRegistry.
 *
 * The Application is created exactly once per test suite: QCoreApplication is
 * a Qt global singleton, so a second Application in the same process would
 * collide when init() constructs a new QCoreApplication.
 */
std::unique_ptr<vn::appfw::Application> bootApplication()
{
    // Nothing this suite writes may reach the user's real data directory: the
    // registration round trip installs a plugin, and installPlugin() writes into
    // <data>/installed.d. Test mode resolves QStandardPaths to a per-user test root
    // (the same trick test_gui uses), and has to be set before the application is
    // built, because the paths are read while it is constructed.
    QStandardPaths::setTestModeEnabled(true);

    static char arg0[] = "test_vsg";
    static char* argv[] = { arg0, nullptr };

    vn::appfw::AppConfig config;
    config.name = "Vine";
    // Unit tests must not read or write the user's real configuration: the
    // builder would otherwise apply Application::defaultConfigFile().
    config.persist_config = false;
    auto app = vn::appfw::createApplication(config, 1, argv);
    EXPECT_NE(app, nullptr);
    if (app != nullptr) {
        // The tests below assert on the discovered entries, so a failure here shows
        // up as a missing entry rather than as a thrown assertion.
        static_cast<void>(app->pluginManager()->loadAll());
    }
    return app;
}

class VsgBackendPluginTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        s_app = bootApplication();
    }

    static std::unique_ptr<vn::appfw::Application> s_app;
};

std::unique_ptr<vn::appfw::Application> VsgBackendPluginTest::s_app;

/// @brief Whether a device case can run at all (a window system and one usable physical device).
bool deviceCaseAvailable()
{
    if (std::getenv("DISPLAY") == nullptr)
    {
        return false;
    }
    const auto probed = vn::vsg::api::probePhysicalDevices();
    return probed.ok && probed.usableCount() != 0;
}

/// @brief Whether @p byte is the display server's spelling of @p value.
///
/// Both the linear value and its sRGB encoding count: the surface's colour space is the platform's, and
/// the two outer bytes are read as a set rather than as named channels (the convention the rewrite's own
/// device cases use).
bool isColourByte(std::uint8_t byte, double value)
{
    const double encoded = value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    const double actual  = static_cast<double>(byte);
    return std::abs(actual - 255.0 * value) <= 6.0 || std::abs(actual - 255.0 * encoded) <= 6.0;
}

/// @brief Whether a pixel is the pass' clear colour: linear (0, 0.25, 0), in either colour space.
bool isGreenClear(const std::array<std::uint8_t, 3>& pixel)
{
    return isColourByte(pixel[1], 0.25) && isColourByte(pixel[0], 0.0) && isColourByte(pixel[2], 0.0);
}

/// @brief Whether a pixel is saturated red: linear (1, 0, 0), in either colour space and either byte order.
bool isRed(const std::array<std::uint8_t, 3>& pixel)
{
    return isColourByte(pixel[1], 0.0) &&
           ((isColourByte(pixel[0], 1.0) && isColourByte(pixel[2], 0.0)) ||
            (isColourByte(pixel[2], 1.0) && isColourByte(pixel[0], 0.0)));
}

}  // namespace

TEST_F(VsgBackendPluginTest, PluginRegistersVsgBackend)
{
    ASSERT_NE(s_app, nullptr);

    auto& registry = vn::graphics::RenderBackendRegistry::instance();
    EXPECT_TRUE(registry.has(u8"vsg"))
        << "gfx_backend_vsg plugin should have registered the 'vsg' backend";
}

TEST_F(VsgBackendPluginTest, CreateBackendByName)
{
    ASSERT_NE(s_app, nullptr);

    auto& registry = vn::graphics::RenderBackendRegistry::instance();

    // The plugin path must produce the REWRITE's facade: the registered name is the switch the whole
    // rewrite exists for (see the design's §11.16bi), and a factory that still created the implementation
    // it replaces would look exactly the same from here. initialize() is intentionally not called here:
    // it creates a real Vulkan window/device, which is unsuitable for a headless unit test - the device
    // case below drives the registered backend through the SDK instead.
    auto backend = registry.create(u8"vsg");
    ASSERT_NE(backend, nullptr);
    EXPECT_NE(dynamic_cast<vn::vsg::VsgBackend*>(backend.get()), nullptr)
        << "the registered 'vsg' backend is the rewrite's facade, not the implementation it replaces";
    EXPECT_TRUE(backend->supportsRenderTargets());
    EXPECT_EQ(backend->nativeHandle(), nullptr) << "no surface was announced on this instance";
}

#if !defined(_WIN32)

/**
 * @brief The backend the plugin REGISTERS comes up on the host's surface and draws through the SDK.
 *
 * `CreateBackendByName` proves the name resolves; this case proves the object it resolves to is a working
 * backend when driven the way the engine drives it: an announced host surface, a session, a pass with one
 * draw, the present - and the picture read back out of the HOST'S OWN window. That is the production path
 * (plugin load -> registry -> create) end to end, and the switch of the name is only proven by its end.
 */
TEST_F(VsgBackendPluginTest, TheRegisteredBackendComesUpOnTheHostsSurfaceAndDraws)
{
    if (!deviceCaseAvailable())
    {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }

    int               screen_index = 0;
    xcb_connection_t* connection   = xcb_connect(nullptr, &screen_index);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);

    constexpr int kWidth  = 128;
    constexpr int kHeight = 96;
    TestHostWindow host(connection, screen, kWidth, kHeight);

    auto& registry = vn::graphics::RenderBackendRegistry::instance();
    auto  backend  = registry.create(u8"vsg");
    ASSERT_NE(backend, nullptr);
    ASSERT_NE(dynamic_cast<vn::vsg::VsgBackend*>(backend.get()), nullptr);

    // The content: a red triangle under a view-block vertex stage (the pair the rewrite's own device cases
    // draw), so the window's pixels say whether the frame went through.
    const vn::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "layout(set = 0, binding = 0, std140) uniform VineViewBlock {\n"
            "    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame; } vb;\n"
            "void main() { gl_Position = vb.view_proj * vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vn::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) out vec4 outColor;\n"
            "void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }

    const vn::intrusive_ptr<Geometry> geometry(new Geometry());
    const vn::intrusive_ptr<vn::Buffer<float>> positions = vn::intrusive_ptr<vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -0.4F, -0.4F, 0.5F, 0.4F, -0.4F, 0.5F, 0.0F, 0.6F, 0.5F }));
    const vn::intrusive_ptr<vn::Buffer<std::uint32_t>> indices =
        vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(
            new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
    geometry->setPositions(positions);
    geometry->setIndices(indices);
    geometry->setRevision(1U);

    const vn::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vn::Colorf(0.2F, 0.3F, 0.4F, 1.0F));

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    const std::vector<RenderCommand> commands{ command };

    // The camera looks half a unit to its right, so the triangle lands in the window's LEFT half.
    vn::intrusive_ptr<vn::graphics::Camera> camera(new vn::graphics::Camera());
    camera->setViewMatrixAsLookAt(vn::math::Vec3d(0.5, 0.0, 1.5), vn::math::Vec3d(0.5, 0.0, 0.0),
                                  vn::math::Vec3d(0.0, 1.0, 0.0));
    camera->setProjectionMatrixAsOrtho(-1.0, 1.0, -1.0, 1.0, 0.5, 4.0);

    const vn::intrusive_ptr<RenderPass> pass(new RenderPass());
    const vn::graphics::ClearPolicy      clear{ vn::Color(0, 64, 0, 255), true };

    backend->setWindowHandle(host.handle());
    ASSERT_TRUE(backend->initialize());
    EXPECT_EQ(backend->nativeHandle(), host.handle()) << "the session adopted the host's window";

    backend->beginFrame();
    backend->beginPass(pass.get());
    backend->setPassOrder(0);
    backend->setRenderTarget(nullptr);
    backend->setClearPolicy(clear);
    backend->setDepthMode(vn::graphics::DepthMode::TestAndWrite);
    backend->render(commands, camera.get());
    backend->endPass();
    backend->endFrame();
    backend->swapBuffers();

    // A presentation reaches the display server asynchronously: two plan-free frames re-present the picture
    // before it is read (the settle the rewrite's own device cases use).
    for (int index = 0; index < 2; ++index)
    {
        backend->beginFrame();
        backend->endFrame();
        backend->swapBuffers();
    }

    const auto left  = host.pixel(kWidth / 4, kHeight / 2);
    const auto right = host.pixel(3 * kWidth / 4, kHeight / 2);
    EXPECT_TRUE(isRed(left)) << "the registered backend drew through the SDK, got ("
                             << static_cast<int>(left[0]) << ", " << static_cast<int>(left[1]) << ", "
                             << static_cast<int>(left[2]) << ")";
    EXPECT_TRUE(isGreenClear(right)) << "and the pass' clear covers the rest";

    backend->shutdown();
    EXPECT_TRUE(host.alive()) << "the host's window is not the backend's to destroy";
}

#endif  // !defined(_WIN32)

/**
 * @brief Verifies the shutdown sequence that makes a disable take effect on the
 *        next start.
 *
 * This suite is the only one that owns an Application, so it is the only place
 * where run() (and therefore Application::shutdown()) can be exercised: the
 * config is written to the file, and reading it back yields the disabled plugin
 * list that the next start resolves against.
 *
 * Runs last in this suite: shutdown() unloads the plugins.
 */
TEST_F(VsgBackendPluginTest, ConfigFileRoundTripPersistsDisabledPlugins)
{
    ASSERT_NE(s_app, nullptr);

    const auto path = std::filesystem::temp_directory_path() / "vine_config_roundtrip_test.json";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // A plugin location outside the program directory, registered like an
    // installed plugin would be.
    const auto user_plugins = std::filesystem::temp_directory_path() / "vine_roundtrip_plugins";
    std::filesystem::remove_all(user_plugins, ec);
    ASSERT_TRUE(std::filesystem::create_directories(user_plugins, ec));

    // A per-user preference: app_shell/test_plugin are application-provided and
    // cannot be disabled, so the round trip records a plugin name of its own.
    EXPECT_TRUE(s_app->pluginManager()->setPluginEnabled(u8"user_plugin_x", false));
    EXPECT_FALSE(s_app->pluginManager()->installPlugin(vn::String(user_plugins.u8string())).empty());
    ASSERT_TRUE(s_app->setConfigFile(path));

    // Stop the main loop from another thread (this suite does not link Qt), so
    // run() returns and performs the shutdown sequence: plugins unload, the bus
    // drains, and the configuration is written.
    std::thread quitter([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (auto* app = vn::appfw::Application::current(); app != nullptr) {
            app->exit(0);
        }
    });
    EXPECT_EQ(s_app->run(), 0);
    quitter.join();
    EXPECT_TRUE(std::filesystem::exists(path)) << "shutdown() must persist the configuration";

    // The next start reads the file back: the disabled plugin is listed there.
    vn::appfw::ConfigManager reloaded;
    ASSERT_TRUE(reloaded.load(vn::String(path.u8string())));
    const auto disabled = reloaded.getStringArray(vn::appfw::PluginManager::disabledConfigKey());
    ASSERT_EQ(disabled.size(), 1u);
    EXPECT_TRUE(disabled[0] == u8"user_plugin_x");

    // The registered location survives too: that is what makes a plugin
    // installed outside the application directory available after a restart.
    const auto installed = s_app->pluginManager()->pluginRegistrations();
    ASSERT_EQ(installed.size(), 1u);
    EXPECT_TRUE(installed[0].path == vn::String(user_plugins.u8string()));
    EXPECT_EQ(installed[0].scope, vn::appfw::PluginScope::User);

    // The preference and the registration survive; the unload kept the metadata
    // (the plugin list is still readable after shutdown).
    EXPECT_FALSE(s_app->pluginManager()->isPluginEnabled(u8"user_plugin_x"));
    EXPECT_TRUE(s_app->pluginManager()->plugin(u8"app_shell") == nullptr);
    const auto entries = s_app->pluginManager()->pluginEntries();
    const auto entry   = std::find_if(entries.begin(), entries.end(),
        [](const vn::appfw::PluginEntry& e) { return e.info.name == u8"app_shell"; });
    ASSERT_NE(entry, entries.end());
    EXPECT_EQ(entry->scope, vn::appfw::PluginScope::BuiltIn);
    EXPECT_FALSE(entry->loaded);

    std::filesystem::remove(path, ec);
    std::filesystem::remove_all(user_plugins, ec);
}

/**
 * @brief The plugins the application ships cannot be disabled or uninstalled.
 *
 * What ships with the application is the application's decision (it can use
 * PluginManager::setSkipList() or simply not build the plugin); a user cannot
 * turn it off or remove it, because that would leave them with a broken program
 * and no way back.
 */
TEST_F(VsgBackendPluginTest, BuiltInPluginsCannotBeDisabledOrUninstalled)
{
    ASSERT_NE(s_app, nullptr);

    auto* pm = s_app->pluginManager();
    ASSERT_NE(pm, nullptr);

    const auto entries = pm->pluginEntries();
    const auto shell   = std::find_if(entries.begin(), entries.end(),
        [](const vn::appfw::PluginEntry& entry) { return entry.info.name == u8"app_shell"; });
    ASSERT_NE(shell, entries.end()) << "app_shell 来自程序目录";
    EXPECT_EQ(shell->scope, vn::appfw::PluginScope::BuiltIn);
    EXPECT_TRUE(shell->enabled);
    EXPECT_FALSE(shell->info.uuid.isNull()) << "插件身份由 VN_DECLARE_PLUGIN 硬编码";

    EXPECT_FALSE(pm->setPluginEnabled(u8"app_shell", false)) << "自带插件不能被禁用";
    EXPECT_TRUE(pm->isPluginEnabled(u8"app_shell"));
    EXPECT_FALSE(pm->uninstallPlugin(u8"app_shell", vn::appfw::PluginScope::User));
    EXPECT_FALSE(pm->uninstallPlugin(u8"app_shell", vn::appfw::PluginScope::AllUsers));
    EXPECT_TRUE(pm->installPlugin(u8"/tmp", vn::appfw::PluginScope::BuiltIn).empty())
        << "a plugin location cannot be registered as application-provided";
}
