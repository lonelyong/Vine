vn_add_library(SYSTEM_TARGET_NAME System)

target_link_libraries(${SYSTEM_TARGET_NAME} PUBLIC vn::Global vn::Core)