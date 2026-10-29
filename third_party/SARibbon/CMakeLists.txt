find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_library(SARibbon STATIC "")
add_library(tp::SARibbon ALIAS SARibbon)

target_sources(SARibbon
    PRIVATE
        SARibbon.cpp
        SARibbon.h
)

set_target_properties(SARibbon PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
    POSITION_INDEPENDENT_CODE ON
    FOLDER third_party
)

target_include_directories(SARibbon
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
)

target_link_libraries(SARibbon
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)