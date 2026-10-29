
add_library(argparse INTERFACE)
add_library(tp::argparse ALIAS argparse)

target_sources(argparse INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/argparse.hpp"
)

set_target_properties(argparse PROPERTIES FOLDER third_party)

target_include_directories(argparse INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)
