set(TEST_NAME test_gui)
file(GLOB SRC_FILE_LIST CONFIGURE_DEPENDS *.cpp)

find_package(Qt6 CONFIG REQUIRED COMPONENTS Core Gui Widgets)

add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)

# 注意：不直接链接 tp::SARibbon —— 它同时被 viAppfwd.dll 静态链接，再链一份
# 会与 DLL 内副本形成两份静态元对象，导致跨 DLL 边界的 qobject_cast 失败。
# 这里只加头文件路径供 static_cast（impl<T>）使用，不用 qobject_cast 断言。
target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/third_party/SARibbon
                                             ${CMAKE_SOURCE_DIR}/third_party/DockingPanes/inc)

target_link_libraries(${TEST_NAME}
    vi::Appfw
    vi::Progress
    Qt6::Core Qt6::Gui Qt6::Widgets)

# Plugins that a current host must refuse, to test the ABI guard end to end: one
# built before the handshake existed, one built against a newer framework. They live
# here (not in src/plugins) because only the tests consume them and they are never
# deployed; the tests load them by the paths injected below.
add_library(vine_plugin_fixture_legacy MODULE fixtures/LegacyPlugin.cpp)
add_library(vine_plugin_fixture_future MODULE fixtures/FuturePlugin.cpp)
add_library(vine_plugin_fixture_chain MODULE fixtures/ChainPlugin.cpp)
add_library(vine_plugin_fixture_loop_a MODULE fixtures/LoopAPlugin.cpp)
add_library(vine_plugin_fixture_loop_b MODULE fixtures/LoopBPlugin.cpp)
foreach(fixture vine_plugin_fixture_legacy vine_plugin_fixture_future vine_plugin_fixture_chain
        vine_plugin_fixture_loop_a vine_plugin_fixture_loop_b)
    set_target_properties(${fixture} PROPERTIES PREFIX "" FOLDER test)
    target_link_libraries(${fixture} PRIVATE vi::Appfw)
endforeach()

target_compile_definitions(${TEST_NAME} PRIVATE
    VINE_LEGACY_ABI_PLUGIN="$<TARGET_FILE:vine_plugin_fixture_legacy>"
    VINE_FUTURE_ABI_PLUGIN="$<TARGET_FILE:vine_plugin_fixture_future>"
    VINE_CHAIN_PLUGIN="$<TARGET_FILE:vine_plugin_fixture_chain>"
    VINE_LOOP_A_PLUGIN="$<TARGET_FILE:vine_plugin_fixture_loop_a>"
    VINE_LOOP_B_PLUGIN="$<TARGET_FILE:vine_plugin_fixture_loop_b>")
add_dependencies(${TEST_NAME} vine_plugin_fixture_legacy vine_plugin_fixture_future vine_plugin_fixture_chain
                 vine_plugin_fixture_loop_a vine_plugin_fixture_loop_b)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

# 这个用例会创建真实的 QWidget：在 ctest 下强制用 offscreen 平台，
# 否则 WSLg / 远程桌面里会弹出一堆窗口（被中断时会留下卡死的窗口）。
# 手跑时仍可自己指定：QT_QPA_PLATFORM=offscreen ./bin/test_gui
set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
