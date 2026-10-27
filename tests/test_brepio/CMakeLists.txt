set(TEST_NAME test_brepio)
file(GLOB SRC_FILE_LIST CONFIGURE_DEPENDS *.cpp)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} PRIVATE vn::BrepIO vn::Geometry)

# The backend is optional: when OCCT is configured the round-trip case builds a real solid, so the test needs the
# headers and the libraries. The package is looked up here as well because imported targets only exist in the directory
# tree that found them. Runtime deployment is vcpkg's job (its applocal step copies the DLLs of linked packages when the
# project is configured with the vcpkg toolchain).
find_package(OpenCASCADE CONFIG QUIET)
if(OpenCASCADE_FOUND)
    target_compile_definitions(${TEST_NAME} PRIVATE VN_BREPIO_HAS_OCCT)
    target_link_libraries(${TEST_NAME} PRIVATE TKPrim TKBRep TKTopAlgo TKernel TKMath TKDESTEP TKDEIGES TKXSBase)
endif()

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
