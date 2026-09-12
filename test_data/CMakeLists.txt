# test_data —— 测试 / 示例素材的搬运规则（数据目录自带的构建代码，不产出任何 target）
#
# 素材要在**二进制旁边**能被找到，两棵树都要：
#
#   <build>/bin/test_data   stage_test_data 目标在构建时同步（用到素材的测试 add_dependencies 它）；
#   <prefix>/test_data      安装树里与 bin/、lib/ 同级。
#
# 两处都只搬**素材**，不含本文件（怎么搬是构建的事，不是数据）。
# 同步由 stamp 驱动：素材没变就一行命令都不跑，空构建仍是空构建。

# 素材集合：CONFIGURE_DEPENDS 让增删文件后重新配置时自动重算。
file(GLOB_RECURSE VINE_TEST_DATA_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*)
list(REMOVE_ITEM VINE_TEST_DATA_FILES ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)

# 构建树里的落点，供消费者（tests/…）复用，别各自拼一遍路径。
set(VINE_TEST_DATA_STAGE_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_data
    CACHE INTERNAL "test_data/ staged for the build tree, beside the binaries")
set(VINE_TEST_DATA_STAMP ${CMAKE_CURRENT_BINARY_DIR}/stage.stamp)

add_custom_command(
    OUTPUT ${VINE_TEST_DATA_STAMP}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR} ${VINE_TEST_DATA_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${VINE_TEST_DATA_STAGE_DIR}/CMakeLists.txt
    COMMAND ${CMAKE_COMMAND} -E touch ${VINE_TEST_DATA_STAMP}
    DEPENDS ${VINE_TEST_DATA_FILES}
    COMMENT "Staging test_data beside the binaries (${VINE_TEST_DATA_STAGE_DIR})"
    VERBATIM)

add_custom_target(stage_test_data ALL DEPENDS ${VINE_TEST_DATA_STAMP})

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ DESTINATION test_data PATTERN CMakeLists.txt EXCLUDE)
