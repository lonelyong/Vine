set(TEST_NAME test_math)
set(SRC_FILE_LIST
	Matrix3x3Test.cpp
	Matrix4x4Test.cpp
	QuaternionTest.cpp
	RectTest.cpp
	VectorTest.cpp
)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vn::Math) 
add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})