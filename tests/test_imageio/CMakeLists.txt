set(TEST_NAME test_imageio)
file(GLOB SRC_FILE_LIST CONFIGURE_DEPENDS *.cpp)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vn::ImageIO vn::Imaging)

# Where the shared test assets live.
#
# test_data/CMakeLists.txt stages the assets beside the binaries (<build>/bin/test_data, target
# stage_test_data) and installs them beside bin/ and lib/, so this test reads THAT copy: no source tree
# and no install are needed at run time (on Windows the test binary would otherwise only work when the
# source tree happens to be there).
# A test that needs an asset then joins VINE_TEST_DATA_DIR with a relative path.
target_compile_definitions(${TEST_NAME} PRIVATE VINE_TEST_DATA_DIR="${VINE_TEST_DATA_STAGE_DIR}")
add_dependencies(${TEST_NAME} stage_test_data)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
