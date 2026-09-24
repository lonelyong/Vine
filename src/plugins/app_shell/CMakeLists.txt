find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg)
qt_standard_project_setup()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

vn_add_plugin(APP_SHELL_TARGET app_shell)

set_target_properties(${APP_SHELL_TARGET} PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

# vn::ImageIO: the demo's cube-mapped box decodes its six faces from test_data/images (a shipped
# asset, not a generated one), which is what this module is for.
# vn::System: Process::currentExecutablePath(), which is half of the asset lookup (see
# demoAssetDirectory: the assets sit beside the executable, wherever it was launched from).
target_link_libraries(${APP_SHELL_TARGET} PRIVATE vn::Appfw vn::Graphics vn::ImageIO vn::Logging vn::System Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)
