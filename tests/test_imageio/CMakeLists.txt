set(TEST_NAME test_imageio)
file(GLOB SRC_FILE_LIST CONFIGURE_DEPENDS *.cpp)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vi::ImageIO vi::Imaging)

# Where the shared test assets live.
#
# This is the first test that needs files off disk, so it also establishes the
# convention for the rest: point at the SOURCE tree's test_data/, because the
# build tree never copies it (and v_add_library's glob only covers sources).
# A test that needs an asset then joins VINE_TEST_DATA_DIR with a relative path.
target_compile_definitions(${TEST_NAME} PRIVATE VINE_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test_data")

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
