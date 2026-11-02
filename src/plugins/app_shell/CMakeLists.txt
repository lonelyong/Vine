find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg)
# lrelease only (never lupdate at build time): optional, so a Qt install without LinguistTools still builds.
find_package(Qt6 CONFIG QUIET COMPONENTS LinguistTools)
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

# vn::Graphics: the central view is a RenderControl; the shell drives its session (engine, view, camera, scene) but
# draws nothing into it - content plugins (demo_plugin) bring their own dependencies for that.
target_link_libraries(${APP_SHELL_TARGET} PRIVATE vn::Appfw vn::Graphics vn::Logging Qt6::Core Qt6::Gui Qt6::Widgets
    Qt6::Svg)

# The shell's own UI text: res/translations/app_shell_<locale>.ts, compiled into :/i18n/app_shell_<locale>.qm. The id is
# the tr() context and the file-name prefix (see .ai/design/appfw-translation.md §2.10).
vn_add_translations(${APP_SHELL_TARGET} CONTEXT app_shell TS_FILES res/translations/app_shell_en.ts)
