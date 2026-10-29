
add_library(nlohmann INTERFACE)
add_library(tp::nlohmann ALIAS nlohmann)

set_target_properties(nlohmann PROPERTIES FOLDER third_party)

target_sources(nlohmann INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include/nlohmann/json.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/nlohmann/json_fwd.hpp"
)

target_include_directories(nlohmann INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)
