find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()

vn_add_plugin(MODEL_VIEWER_TARGET model_viewer)

set_target_properties(${MODEL_VIEWER_TARGET} PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

# vn::Appfw: the plugin base, the document surface (Document / DocumentManager) and the dock panel API (Control).
# vn::Geometry: the mesh a model document holds. IndexedTriangleMesh is where the two numbers the info panel shows
#               come from (vertexCount / triangleCount) - not a re-count of something we built ourselves.
# vn::Graphics: the view IS the renderer (ModelRenderView): its own SceneView on the shell's shared render control,
#               and the Geometry/Scene/Material a mesh becomes. Not a second render session - see the class note.
# vn::MeshIO: MeshLoader, for the opener that reads a mesh file (the "parsed from disk" half of the payload story).
# vn::Logging: VN_LOGW for the "opener refused" paths.
# No ImageIO/System here, so the RPATH note in VinePluginHelper.cmake has nothing to catch.
target_link_libraries(${MODEL_VIEWER_TARGET} PRIVATE vn::Appfw vn::Geometry vn::Graphics vn::Logging vn::MeshIO
    Qt6::Core Qt6::Gui Qt6::Widgets)
