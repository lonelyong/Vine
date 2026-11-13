if(MSVC)
    add_compile_options(/utf-8 /EHsc /Zc:preprocessor /Zc:__cplusplus)
endif()

add_subdirectory(argparse)
add_subdirectory(boolinq)
add_subdirectory(DockingPanes)
add_subdirectory(SARibbon)
add_subdirectory(stb)
add_subdirectory(volk)
add_subdirectory(tinyxml2)
add_subdirectory(nlohmann)

# glslang is NOT added here: it is provisioned at the TOP LEVEL by vn_provide_glslang() (see
# cmake/VineGlslang.cmake), because an IMPORTED target created in this directory would be invisible to src/.
# It serves both shader-compiling backends (gfx_backend_vsg through vsg's ShaderCompiler, gfx_backend_vk directly).