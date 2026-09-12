v_add_library(IMAGING_TARGET_NAME Imaging)

target_link_libraries(${IMAGING_TARGET_NAME} PUBLIC vi::Core vi::Global)
