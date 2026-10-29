add_library(tinyxml2 STATIC "")
add_library(tp::tinyxml2 ALIAS tinyxml2)

target_sources(tinyxml2
    PRIVATE
        tinyxml2.h
        tinyxml2.cpp
)

set_target_properties(tinyxml2 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER third_party
)

target_include_directories(tinyxml2
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
)
 