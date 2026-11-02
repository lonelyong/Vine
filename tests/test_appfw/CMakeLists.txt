set(TEST_NAME test_appfw)
file(GLOB SRC_FILE_LIST CONFIGURE_DEPENDS *.cpp)

find_package(Qt6 CONFIG REQUIRED COMPONENTS Core)
# lrelease only (never lupdate): optional, so a Qt install without LinguistTools still builds the tests.
find_package(Qt6 CONFIG QUIET COMPONENTS LinguistTools)

add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)

# 无头宿主的用例：整条启动路径都不碰窗口系统，所以只链 Qt6::Core（appfw 自己带
# Qt6::Gui/Widgets，那是它的实现细节，不是这个宿主要求的）。
# vn::Logging 是为了用 LogSink 抓告警行（身份冲突/注册文件不符都是诊断，只能从日志断言）。
target_link_libraries(${TEST_NAME}
    vn::Appfw
    vn::Logging
    Qt6::Core)

# 夹具插件：用例把它拷进一个空目录当"自带插件目录"，用来钉住"框架在启动阶段加载插件"
# （宿主不写 loadAll()）。同一套做法见 test_gui 的 vn_plugin_fixture_*。
add_library(vn_plugin_fixture_headless_boot MODULE fixtures/HeadlessBootPlugin.cpp)
set_target_properties(vn_plugin_fixture_headless_boot PROPERTIES PREFIX "" FOLDER test)
target_link_libraries(vn_plugin_fixture_headless_boot PRIVATE vn::Appfw)
target_compile_definitions(${TEST_NAME} PRIVATE
    VN_HEADLESS_BOOT_PLUGIN="$<TARGET_FILE:vn_plugin_fixture_headless_boot>"
    VN_TWIN_IDENTITY_PLUGIN="$<TARGET_FILE:vn_plugin_fixture_twin_identity>"
    VN_I18N_PLUGIN="$<TARGET_FILE:vn_plugin_fixture_i18n>"
    VN_THROWING_PLUGIN="$<TARGET_FILE:vn_plugin_fixture_throwing>"
    VN_RESERVED_CONTEXT_PLUGIN="$<TARGET_FILE:vn_plugin_fixture_reserved_context>")
add_dependencies(${TEST_NAME} vn_plugin_fixture_headless_boot)

# 第二份夹具：与上一份**同一个 uuid**、名字不同（见文件头说明）——用来钉"两个插件声明同一个身份"
# 的告警：名字不同，除了 uuid 没有任何检查会发现它们其实是同一个插件。
add_library(vn_plugin_fixture_twin_identity MODULE fixtures/TwinIdentityPlugin.cpp)
set_target_properties(vn_plugin_fixture_twin_identity PROPERTIES PREFIX "" FOLDER test)
target_link_libraries(vn_plugin_fixture_twin_identity PRIVATE vn::Appfw)
add_dependencies(${TEST_NAME} vn_plugin_fixture_twin_identity)

# Third fixture: a plugin that carries its own catalog, embedded into its own DLL as :/i18n/i18n_fixture_en.qm.
# It pins where a plugin's translations come from: the loader installs it before the plugin's code runs, and no
# directory is scanned (the plugin id is the file-name prefix).
add_library(vn_plugin_fixture_i18n MODULE fixtures/TranslationPlugin.cpp)
set_target_properties(vn_plugin_fixture_i18n PROPERTIES PREFIX "" FOLDER test)
# Qt6::Core is required by the generated resource code (qRegisterResourceData); vn::Appfw links Qt privately.
target_link_libraries(vn_plugin_fixture_i18n PRIVATE vn::Appfw Qt6::Core)
vn_add_translations(vn_plugin_fixture_i18n CONTEXT i18n_fixture TS_FILES fixtures/res/translations/i18n_fixture_en.ts)
add_dependencies(${TEST_NAME} vn_plugin_fixture_i18n)

# Fourth fixture: a plugin whose construction throws, carrying its own catalog. It pins the rollback of a failed batch
# load - the catalog is installed before create(), so a plugin that never comes up must not leave it behind.
add_library(vn_plugin_fixture_throwing MODULE fixtures/ThrowingPlugin.cpp)
set_target_properties(vn_plugin_fixture_throwing PROPERTIES PREFIX "" FOLDER test)
target_link_libraries(vn_plugin_fixture_throwing PRIVATE vn::Appfw Qt6::Core)
vn_add_translations(vn_plugin_fixture_throwing CONTEXT i18n_throwing TS_FILES fixtures/res/translations/i18n_throwing_en.ts)
add_dependencies(${TEST_NAME} vn_plugin_fixture_throwing)

# The framework owns the appfw translation context; plugin metadata must not claim it.
add_library(vn_plugin_fixture_reserved_context MODULE fixtures/ReservedTranslationContextPlugin.cpp)
set_target_properties(vn_plugin_fixture_reserved_context PROPERTIES PREFIX "" FOLDER test)
target_link_libraries(vn_plugin_fixture_reserved_context PRIVATE vn::Appfw)
add_dependencies(${TEST_NAME} vn_plugin_fixture_reserved_context)

# Fifth fixture: the same context and locale as i18n_fixture, translated differently, kept as a LOOSE .qm (never
# embedded). The test copies it beside a copy of the plugin library, which is how "a file beside the module wins over the
# copy inside it" is pinned. Without LinguistTools the file is not produced and the case skips itself.
set(VN_OVERRIDE_QM "${CMAKE_CURRENT_BINARY_DIR}/override/i18n_fixture_en.qm")
if(TARGET Qt6::lrelease)
    add_custom_command(
        OUTPUT "${VN_OVERRIDE_QM}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/override"
        COMMAND Qt6::lrelease "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/res/translations/i18n_fixture_override.ts" -qm "${VN_OVERRIDE_QM}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/res/translations/i18n_fixture_override.ts"
        COMMENT "lrelease i18n_fixture_override.ts"
        VERBATIM)
    add_custom_target(vn_plugin_fixture_override_qm DEPENDS "${VN_OVERRIDE_QM}")
    add_dependencies(${TEST_NAME} vn_plugin_fixture_override_qm)
endif()
target_compile_definitions(${TEST_NAME} PRIVATE VN_I18N_OVERRIDE_QM="${VN_OVERRIDE_QM}")

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
