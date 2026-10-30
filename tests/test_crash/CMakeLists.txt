set(TEST_NAME test_crash)
file(GLOB SRC_FILE_LIST CONFIGURE_DEPENDS *.cpp)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vn::Crash vn::Core)

# The image table is resolved by a module-internal function (src/base/crash/src/CrashDetail.hpp): the mapping-file parser is
# pure, so it is worth pinning directly instead of through a real crash. Same precedent as test_asyncqt and test_gui.
target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src/base/crash/src)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
