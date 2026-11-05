set(TEST_NAME test_geom2d)
set(SRC_FILE_LIST
        ArcCurveTest.cpp
        BoundsTest.cpp
        ConicCurvesTest.cpp
        CurveBasicsTest.cpp
        CurveContractTest.cpp
        CurveProjectionTest.cpp
        EllipseCurveTest.cpp
)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vn::Geom2D)
add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
