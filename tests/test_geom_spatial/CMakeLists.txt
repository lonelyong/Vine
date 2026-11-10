set(TEST_NAME test_geom_spatial)
set(SRC_FILE_LIST
	AabbTest.cpp
	BSplineBasisTest.cpp
	BvhBuildTest.cpp
	BvhBuilderContractTest.cpp
	BvhQueryTest.cpp
	CurveCurvatureTest.cpp
	CurveCurveDistanceTest.cpp
	CurveDistanceTest.cpp
	CurveLengthTest.cpp
	CurveSamplingTest.cpp
	CurveSurfaceTest.cpp
	GeometryKindsTest.cpp
	LinePlaneTest.cpp
	MeshBvhTest.cpp
	ObbFitTest.cpp
	ObbTest.cpp
	PointCloudTest.cpp
	QuadratureTest.cpp
	RayTest.cpp
	RootFindingTest.cpp
	SolidDistanceTest.cpp
	SolidIntersectionTest.cpp
	SolidTessellationTest.cpp
	SolidTest.cpp
	SurfaceQueryTest.cpp
	TriangleBvhTest.cpp
)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vn::Geom vn::Models)
add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
