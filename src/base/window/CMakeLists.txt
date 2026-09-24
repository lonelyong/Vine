vn_add_library(WINDOW_TARGET_NAME Window)

target_link_libraries(${WINDOW_TARGET_NAME} PUBLIC vn::Core vn::Global)
