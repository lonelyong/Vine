find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
# lrelease only (never lupdate at build time): optional, so a Qt install without LinguistTools still builds.
find_package(Qt6 CONFIG QUIET COMPONENTS LinguistTools)
qt_standard_project_setup()

vn_add_plugin(MODEL_VIEWER_TARGET model_viewer)

set_target_properties(${MODEL_VIEWER_TARGET} PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

# The shading-mode icons are plugin-local resources, compiled into this module. The prefix is scoped to
# model_viewer so a host that ships its own :/icons cannot collide with them.
target_sources(${MODEL_VIEWER_TARGET} PRIVATE res/res.qrc)

# vn::Appfw: the plugin base, the document surface (Document / DocumentManager) and the dock panel API (Control).
# vn::Geom3D: the mesh a model document holds. TriangleMesh is where the two numbers the info panel shows
#               come from (vertexCount / triangleCount) - not a re-count of something we built ourselves.
# vn::Graphics: the view IS the renderer (ModelRenderView): its own render control and its own SceneView on it,
#               and the Geometry/Scene/Material a mesh becomes. Not a second render session - see the class note.
# vn::MeshIO: MeshLoader, for the opener that reads a mesh file (the "parsed from disk" half of the payload story).
# vn::Logging: VN_LOGW for the "opener refused" paths.
# No ImageIO/System here, so the RPATH note in VinePluginHelper.cmake has nothing to catch.
target_link_libraries(${MODEL_VIEWER_TARGET} PRIVATE vn::Appfw vn::Geom3D vn::Models vn::Graphics vn::Logging vn::MeshIO
    Qt6::Core Qt6::Gui Qt6::Widgets)

# The plugin's own UI text: res/translations/model_viewer_<locale>.ts, compiled into :/i18n/model_viewer_<locale>.qm.
# The loader installs it before load() runs and removes it on unload; the id here is the tr() context and the
# file-name prefix (see .ai/design/appfw-translation.md §2.10).
vn_add_translations(${MODEL_VIEWER_TARGET} CONTEXT model_viewer TS_FILES res/translations/model_viewer_zh_CN.ts)
