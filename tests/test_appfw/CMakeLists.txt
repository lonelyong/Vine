set(TEST_NAME test_appfw)
file(GLOB SRC_FILE_LIST CONFIGURE_DEPENDS *.cpp)

find_package(Qt6 CONFIG REQUIRED COMPONENTS Core)

add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)

# 无头宿主的用例：整条启动路径都不碰窗口系统，所以只链 Qt6::Core（appfw 自己带
# Qt6::Gui/Widgets，那是它的实现细节，不是这个宿主要求的）。
target_link_libraries(${TEST_NAME}
    vn::Appfw
    Qt6::Core)

# 夹具插件：用例把它拷进一个空目录当"自带插件目录"，用来钉住"框架在启动阶段加载插件"
# （宿主不写 loadAll()）。同一套做法见 test_gui 的 vine_plugin_fixture_*。
add_library(vine_plugin_fixture_headless_boot MODULE fixtures/HeadlessBootPlugin.cpp)
set_target_properties(vine_plugin_fixture_headless_boot PROPERTIES PREFIX "" FOLDER test)
target_link_libraries(vine_plugin_fixture_headless_boot PRIVATE vn::Appfw)
target_compile_definitions(${TEST_NAME} PRIVATE
    VINE_HEADLESS_BOOT_PLUGIN="$<TARGET_FILE:vine_plugin_fixture_headless_boot>")
add_dependencies(${TEST_NAME} vine_plugin_fixture_headless_boot)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
