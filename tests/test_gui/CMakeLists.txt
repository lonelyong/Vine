set(TEST_NAME test_gui)
file(GLOB SRC_FILE_LIST CONFIGURE_DEPENDS *.cpp)

find_package(Qt6 CONFIG REQUIRED COMPONENTS Core Gui Widgets)

add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)

# 注意：不直接链接 tp::SARibbon —— 它同时被 viAppfwd.dll 静态链接，再链一份
# 会与 DLL 内副本形成两份静态元对象，导致跨 DLL 边界的 qobject_cast 失败。
# 这里只加头文件路径供 static_cast（impl<T>）使用，不用 qobject_cast 断言。
target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/third_party/SARibbon
                                             ${CMAKE_SOURCE_DIR}/third_party/DockingPanes/inc
                                             # PreviewFit.hpp is a pure-arithmetic helper of the demo
                                             # (it lives in the plugin's src/, like the private appfw
                                             # header below) so its unit test can include it directly.
                                             ${CMAKE_SOURCE_DIR}/src/plugins/demo_plugin/src
                                             # ModelDocument.hpp / ModelInfoPanel.hpp / TestModelData.hpp /
                                             # MeshFileOpener.hpp of the model_viewer plugin: the info panel is a
                                             # header-only Control (no metadata, no signals) and the file opener is
                                             # a header-only function precisely so this test can drive them
                                             # without loading the plugin - the plugin's load() registers exactly
                                             # those functions.
                                             ${CMAKE_SOURCE_DIR}/src/plugins/model_viewer/src
                                             # ConsoleUserIO is appfw-private (it lives in
                                             # src/, not sdk/): the headless progress test
                                             # constructs it directly, like test_asyncqt
                                             # includes the private Qt coroutine glue.
                                             ${CMAKE_SOURCE_DIR}/src/fw/appfw/src)

# The model_viewer cases register the plugin's OWN openers, and registerOpener<T> needs T's metadata
# (it reads the Type to bind the payload to a document type). A payload type's metadata must exist
# exactly once per process - a second Type for the same C++ type throws ITEM_ALREADY_EXISTS - so the
# case takes it from the plugin's source rather than loading the plugin. That is only safe because
# nothing here dlopens model_viewer (the sandbox cases load app_shell / test_plugin): if one ever
# does, the payload metadata has to move out of this target again.
# (test_vsg compiles gfx_backend_vsg sources for the same kind of reason.)
target_sources(${TEST_NAME}
    PRIVATE ${CMAKE_SOURCE_DIR}/src/plugins/model_viewer/src/ModelPayload.cpp
            # The "display mode" commands: their cases register and run them through the REAL command
            # manager (app->commandManager()->executeCommandAndWait), which is what the layer they live in
            # is for, so the classes' metadata has to exist in this target - same reason as the payload
            # above, and equally safe because nothing here dlopens model_viewer.
            ${CMAKE_SOURCE_DIR}/src/plugins/model_viewer/src/commands/ShadingModeCommands.cpp)

# A plugin loaded from OUTSIDE the application directory (which is exactly what UserPluginSandbox
# copies into /tmp/vn_test_plugins) cannot use its own RUNPATH to find anything: the loader only
# has the libraries the HOST process already loaded, and dlopen reports the plugin itself as
# "missing dependency: <plugin>" without naming the library. The sandbox loads app_shell and
# test_plugin, and app_shell needs Appfw / Graphics / Qt (:Logging comes through Appfw) - all of
# which this target links. The demo plugin is NOT loaded here and is the one plugin that needs more
# (see cmake/VinePluginHelper.cmake): add vn::ImageIO / vn::System to this list if a case ever loads
# it from a sandbox.
target_link_libraries(${TEST_NAME}
    vn::Appfw
    vn::Progress
    vn::Geometry
    vn::Graphics
    Qt6::Core Qt6::Gui Qt6::Widgets
    # The command cases assert on the LOG LEVEL a status is recorded with (a NotApplicable result
    # must not be an error), so they attach a LogSink - same reason test_appfw links it.
    vn::Logging
    # The model_viewer cases drive its OPENERS, and the file opener calls MeshLoader: the plugin's own
    # registration just forwards to that header-only function, so this is the only extra library the
    # viewer's "open a model file" path needs here.
    vn::MeshIO)

# Plugins that a current host must refuse, to test the ABI guard end to end: one
# built before the handshake existed, one built against a newer framework. They live
# here (not in src/plugins) because only the tests consume them and they are never
# deployed; the tests load them by the paths injected below.
add_library(vn_plugin_fixture_legacy MODULE fixtures/LegacyPlugin.cpp)
add_library(vn_plugin_fixture_future MODULE fixtures/FuturePlugin.cpp)
add_library(vn_plugin_fixture_chain MODULE fixtures/ChainPlugin.cpp)
add_library(vn_plugin_fixture_loop_a MODULE fixtures/LoopAPlugin.cpp)
add_library(vn_plugin_fixture_loop_b MODULE fixtures/LoopBPlugin.cpp)
foreach(fixture vn_plugin_fixture_legacy vn_plugin_fixture_future vn_plugin_fixture_chain
        vn_plugin_fixture_loop_a vn_plugin_fixture_loop_b)
    set_target_properties(${fixture} PROPERTIES PREFIX "" FOLDER test)
    target_link_libraries(${fixture} PRIVATE vn::Appfw)
endforeach()

target_compile_definitions(${TEST_NAME} PRIVATE
    VN_LEGACY_ABI_PLUGIN="$<TARGET_FILE:vn_plugin_fixture_legacy>"
    VN_FUTURE_ABI_PLUGIN="$<TARGET_FILE:vn_plugin_fixture_future>"
    VN_CHAIN_PLUGIN="$<TARGET_FILE:vn_plugin_fixture_chain>"
    VN_LOOP_A_PLUGIN="$<TARGET_FILE:vn_plugin_fixture_loop_a>"
    VN_LOOP_B_PLUGIN="$<TARGET_FILE:vn_plugin_fixture_loop_b>")
add_dependencies(${TEST_NAME} vn_plugin_fixture_legacy vn_plugin_fixture_future vn_plugin_fixture_chain
                 vn_plugin_fixture_loop_a vn_plugin_fixture_loop_b)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

# 这个用例会创建真实的 QWidget：在 ctest 下强制用 offscreen 平台，
# 否则 WSLg / 远程桌面里会弹出一堆窗口（被中断时会留下卡死的窗口）。
# 手跑时仍可自己指定：QT_QPA_PLATFORM=offscreen ./bin/test_gui
set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# offscreen 插件得真在树里，否则 Qt 在**任何用例跑起来之前**就 abort
# （qt.qpa.plugin: Could not find the Qt platform plugin "offscreen"），下面那条 CTest 永远红 ——
# 实测：本机 Release 树只有 windeployqt 部署的 qwindows.dll，DeployQt 那个目标没人跑。
# 所以这里自己带上它：插件与 Qt 库同目录（测试可执行文件旁边），不依赖"有人记得跑 DeployQt"。
vn_find_qt_offscreen_plugin_dir(VN_QT_OFFSCREEN_PLUGIN_DIR)
if (VN_QT_OFFSCREEN_PLUGIN_DIR)
    add_custom_command(
            TARGET ${TEST_NAME}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${TEST_NAME}>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VN_QT_OFFSCREEN_PLUGIN_DIR}/${VN_QT_OFFSCREEN_PLUGIN_NAME}"
                    "$<TARGET_FILE_DIR:${TEST_NAME}>/platforms/"
            COMMENT "--------Deploy the offscreen platform plugin (ctest uses QT_QPA_PLATFORM=offscreen)")
else ()
    message(WARNING "${TEST_NAME}: no offscreen platform plugin found; ctest will not start this suite "
            "(QT_QPA_PLATFORM=offscreen)")
endif ()
