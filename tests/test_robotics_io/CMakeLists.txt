set(TEST_NAME test_robotics_io)
file(GLOB SRC_FILE_LIST CONFIGURE_DEPENDS *.cpp)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vn::RoboticsIO vn::RoboticsCore tinyxml2)

# Where the shared test assets live: test_data/CMakeLists.txt stages them beside the binaries
# (<build>/bin/test_data), so a test that needs one joins VINE_TEST_DATA_DIR with a relative path.
target_compile_definitions(${TEST_NAME} PRIVATE VINE_TEST_DATA_DIR="${VINE_TEST_DATA_STAGE_DIR}")
add_dependencies(${TEST_NAME} stage_test_data)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
