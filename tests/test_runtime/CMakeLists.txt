set(TEST_NAME test_runtime)
file(GLOB SRC_FILE_LIST CONFIGURE_DEPENDS *.cpp)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vi::Runtime vi::Core)

# A real shared library for the loader tests. Loading the test executable itself cannot work on
# Linux: it is a PIE, and glibc refuses to dlopen a position-independent executable. Loading a
# system library by name would exercise the dynamic linker rather than this loader, so the tests
# load this fixture instead. It sits in its own directory so the glob above does not compile it
# into the test as well.
add_library(runtime_test_fixture SHARED fixture/fixture.cpp)
set_target_properties(runtime_test_fixture PROPERTIES FOLDER test)
target_compile_definitions(${TEST_NAME} PRIVATE "V_RUNTIME_FIXTURE=\"$<TARGET_FILE:runtime_test_fixture>\"")

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
