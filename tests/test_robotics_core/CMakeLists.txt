set(TEST_NAME test_robotics_core)
set(SRC_FILE_LIST
	DHTransformConverterTest.cpp
	FrameTest.cpp
	JacobianIKSolverTest.cpp
	PieperIKSolverTest.cpp
	QStateTest.cpp
	QTest.cpp
	SerialKinematicsTest.cpp
	WorkcellTest.cpp
)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vn::RoboticsCore)
add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
