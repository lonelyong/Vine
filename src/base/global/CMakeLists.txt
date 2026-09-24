# find_package(icu COMPONENTS uc)

vn_add_library(GLOBAL_TARGET_NAME Global)

target_compile_definitions(${GLOBAL_TARGET_NAME} INTERFACE VN_ROOT_NS=vn)