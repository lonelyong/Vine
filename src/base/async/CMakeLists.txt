vn_add_library(ASYNC_TARGET_NAME Async)

target_link_libraries(${ASYNC_TARGET_NAME} INTERFACE vn::Global vn::Core)
