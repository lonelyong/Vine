
add_library(boolinq INTERFACE)
add_library(tp::boolinq ALIAS boolinq)

target_sources(boolinq INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/boolinq.h"
)

set_target_properties(boolinq PROPERTIES FOLDER third_party)

target_include_directories(boolinq INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)
