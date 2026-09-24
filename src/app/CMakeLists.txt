set(APP_TARGET_NAME Vine)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()

file(GLOB_RECURSE SRC_FILE_LIST LIST_DIRECTORIES false *.cpp)
file(GLOB_RECURSE HEADER_FILE_LIST LIST_DIRECTORIES false *.hpp *.h)
file(GLOB_RECURSE RC_FILE_LIST LIST_DIRECTORIES false *.rc)

qt_add_executable(${APP_TARGET_NAME} WIN32 ${SRC_FILE_LIST} ${HEADER_FILE_LIST} ${RC_FILE_LIST})

if(MSVC)
    target_compile_options(${APP_TARGET_NAME} PRIVATE /utf-8 /EHsc /Zc:preprocessor /Zc:__cplusplus)
endif()

set_target_properties(${APP_TARGET_NAME} PROPERTIES WIN32_EXECUTABLE TRUE MACOSX_BUNDLE TRUE FOLDER vine/app)

target_link_libraries(${APP_TARGET_NAME} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets vn::Appfw vn::Di vn::Global vn::Logging)

install(TARGETS ${APP_TARGET_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    
# add_custom_command(TARGET ${APP_TARGET_NAME} POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E copy_if_different
#         $<TARGET_FILE:Qt6::Core>
#         $<TARGET_FILE:Qt6::Gui>
#         $<TARGET_FILE:Qt6::Widgets>
#         $<TARGET_FILE_DIR:${APP_TARGET_NAME}>)
# set_target_properties(${APP_TARGET_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)

# copy_qt_dependencies(${APP_TARGET_NAME} Qt::Core Qt::Widgets Qt::Gui)

# find_package(Qt6 REQUIRED COMPONENTS Core)
# qt_standard_project_setup()

# qt_add_executable(MyApp main.cpp)

# install(TARGETS MyApp
#     BUNDLE  DESTINATION .
#     RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
# )

# qt_generate_deploy_app_script(
#     TARGET MyApp
#     OUTPUT_SCRIPT deploy_script
#     NO_UNSUPPORTED_PLATFORM_ERROR
# )
# install(SCRIPT ${deploy_script})

