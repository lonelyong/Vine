set(TEST_NAME test_brep)
set(SRC_FILE_LIST
	TessellationTest.cpp
)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vn::Brep)
add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
