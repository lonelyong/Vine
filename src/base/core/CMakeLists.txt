# find_package(icu COMPONENTS uc)

vn_add_library(CORE_TARGET_NAME Core)

target_link_libraries(${CORE_TARGET_NAME} PUBLIC vn::Global)