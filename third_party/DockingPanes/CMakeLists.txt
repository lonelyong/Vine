find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Xml)

file(GLOB_RECURSE HEADER_FILE_LIST *.h)
file(GLOB_RECURSE SRC_FILE_LIST *.cpp)
file(GLOB_RECURSE RC_FILE_LIST *.qrc)
file(GLOB_RECURSE IMG_FILE_LIST *.png)

add_library(DockingPanes STATIC "")
add_library(tp::DockingPanes ALIAS DockingPanes)

target_sources(DockingPanes
    PRIVATE
    ${HEADER_FILE_LIST}
    ${SRC_FILE_LIST}
    ${RC_FILE_LIST})

set_target_properties(DockingPanes PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
    POSITION_INDEPENDENT_CODE ON
    FOLDER third_party
)

target_include_directories(DockingPanes
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/inc>
)

target_link_libraries(DockingPanes
    PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Xml
)
