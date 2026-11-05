# Models — the modelling types a scene or a file describes.
#
# Identity and storage: Shape, Primitive, Model, the mesh family, BrepShape, PointCloud and the
# materials. Geometry stays below this module; the only value that crosses the boundary is the
# analytic solid a primitive builds on demand (solid()), plus mesh triangulation, which turns a
# geometry value into a model mesh.
#
# Geometry is PUBLIC because the headers speak in Aabb / Solid types.
vn_add_library(MODELS_TARGET_NAME Models)

target_link_libraries(${MODELS_TARGET_NAME} PUBLIC vn::Geom3D)
