vn_add_library(DI_TARGET_NAME Di)

target_link_libraries(${DI_TARGET_NAME} PUBLIC vn::Global)
target_link_libraries(${DI_TARGET_NAME} PUBLIC vn::Core)