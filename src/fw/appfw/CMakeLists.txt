
find_package(Qt6 CONFIG REQUIRED COMPONENTS Core Gui Widgets Svg)
qt_standard_project_setup()

set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

vn_add_library(APPFW_TARGET_NAME Appfw)

# The version a plugin reports as "built against" (PluginAbi::framework_version).
# PUBLIC: the application, the plugins and the tests all link this target and
# therefore compile the same value, which is the point - a plugin reports the
# framework version it was built with, not the one it is loaded into.
target_compile_definitions(${APPFW_TARGET_NAME} PUBLIC VN_APPFW_VERSION="${PROJECT_VERSION}")

set_target_properties(${APPFW_TARGET_NAME} PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

target_link_libraries(${APPFW_TARGET_NAME}
    PUBLIC vn::Async vn::Core vn::Di vn::Math vn::Global vn::Graphics vn::Window vn::Progress
    PRIVATE
    vn::Logging
    vn::Runtime
    tp::SARibbon
    tp::DockingPanes
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)

# Application factories: AppBuilder.cpp, GuiAppBuilder.cpp.
