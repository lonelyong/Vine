
find_package(Qt6 CONFIG REQUIRED COMPONENTS Core Gui Widgets Svg)
# lrelease only (never lupdate at build time): optional, so a Qt install without LinguistTools still builds.
find_package(Qt6 CONFIG QUIET COMPONENTS LinguistTools)
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

# The framework's own UI text: res/translations/appfw_<locale>.ts (sources of truth, in git) compiled into
# :/i18n/appfw_<locale>.qm. Only English is translated - Chinese is the source language.
vn_add_translations(${APPFW_TARGET_NAME} CONTEXT appfw TS_FILES res/translations/appfw_en.ts)

target_link_libraries(${APPFW_TARGET_NAME}
    PUBLIC vn::Async vn::Core vn::Di vn::Math vn::Global vn::Graphics vn::Window vn::Progress
    PRIVATE
    vn::Logging
    vn::Runtime
    tp::SARibbon
    tp::DockingPanes
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)

# Application factories: AppBuilder.cpp, GuiAppBuilder.cpp.
