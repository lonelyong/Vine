 
vn_add_library(APPFW_TARGET_NAME RoboticsCore)

target_link_libraries(${APPFW_TARGET_NAME}
    PUBLIC vn::Core vn::Math vn::Geometry vn::Global)
