set(TEST_NAME test_asyncqt)
file(GLOB SRC_FILE_LIST CONFIGURE_DEPENDS *.cpp)

find_package(Qt6 CONFIG REQUIRED COMPONENTS Core)

add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)

# The Qt coroutine glue is private to Appfw (headers live in src/async).
target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src/fw/appfw/src)

target_link_libraries(${TEST_NAME}
    vn::Appfw
    vn::Async
    Qt6::Core)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
