set(TEST_NAME test_graphics)
set(SRC_FILE_LIST
	GraphicsTest.cpp
	ChannelSliceTest.cpp
)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vi::Graphics vi::Geometry)
add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
