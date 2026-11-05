set(TEST_NAME test_geom3d)
set(SRC_FILE_LIST
	AabbTest.cpp
	CurveDistanceTest.cpp
	CurveSurfaceTest.cpp
	GeometryKindsTest.cpp
	LinePlaneTest.cpp
	ObbFitTest.cpp
	ObbTest.cpp
	PointCloudTest.cpp
	RayTest.cpp
	SolidDistanceTest.cpp
	SolidIntersectionTest.cpp
	SolidTessellationTest.cpp
	SolidTest.cpp
	SurfaceQueryTest.cpp
)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vn::Geom3D vn::Models)
add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
