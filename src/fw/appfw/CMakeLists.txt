
find_package(Qt6 CONFIG REQUIRED COMPONENTS Core Gui Widgets Svg)
qt_standard_project_setup()

set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

v_add_library(APPFW_TARGET_NAME Appfw)

set_target_properties(${APPFW_TARGET_NAME} PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

target_link_libraries(${APPFW_TARGET_NAME}
    PUBLIC vi::Async vi::Core vi::Di vi::Math vi::Global vi::Graphics vi::Window
    PRIVATE
    vi::Logging
    vi::Progress
    vi::Runtime
    tp::SARibbon
    tp::DockingPanes
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)

# Application factories: AppBuilder.cpp, GuiAppBuilder.cpp.
