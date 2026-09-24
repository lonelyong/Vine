vn_add_library(RUNTIME_TARGET_NAME Runtime)

target_link_libraries(${RUNTIME_TARGET_NAME} PUBLIC vn::Global vn::Core)
# CMAKE_DL_LIBS provides -ldl on Linux for dlopen/dlsym; empty on Windows.
target_link_libraries(${RUNTIME_TARGET_NAME} PRIVATE ${CMAKE_DL_LIBS})