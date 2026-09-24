vn_add_library(MATH_TARGET_NAME Math)

target_link_libraries(${MATH_TARGET_NAME} PUBLIC vn::Global)

# Enable LTO and emit link-time inline remarks.
set_property(TARGET ${MATH_TARGET_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Emit optimization remarks for inlining decisions.
    # Requires -O2 or higher; works with both Debug and Release.
    target_compile_options(${MATH_TARGET_NAME} PRIVATE -Rpass=inline -Rpass-missed=inline)
    target_link_options(${MATH_TARGET_NAME} PRIVATE $<$<CONFIG:Release>:-Rpass=inline> $<$<CONFIG:Release>:-Rpass-missed=inline>)
endif()
