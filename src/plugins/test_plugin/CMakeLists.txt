find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

vn_add_plugin(TEST_PLUGIN_TARGET test_plugin)

set_target_properties(${TEST_PLUGIN_TARGET} PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

target_link_libraries(${TEST_PLUGIN_TARGET} PRIVATE vn::Appfw vn::Graphics vn::Geometry vn::Progress vn::Logging Qt6::Core Qt6::Gui Qt6::Widgets)
