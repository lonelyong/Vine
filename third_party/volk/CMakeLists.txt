# volk — meta-loader for Vulkan (header v363, upstream commit 54fc0d7a, 2026-09-18),
# vendored.
#
# Vendored rather than fetched: volk is two files that carry their own license, and
# the two build options this backend depends on (`VOLK_NAMESPACE`, and the
# device-prototype hiding) are compile definitions on a target we have to declare
# ourselves anyway. A FetchContent of it would add a network step to a plugin whose
# other dependencies already need one, for no gain.
#
# TWO OPTIONS, BOTH LOAD-BEARING (see volk's README, "Configuration"):
#
#  * VOLK_NAMESPACE — without it volk defines every Vulkan entry point as a GLOBAL
#    variable named after the function (`vkCreateDevice`, `vkQueueSubmit`, ...) and its
#    own README says that "can result in symbol conflicts if some libraries in the
#    application are still linking to Vulkan libraries directly". That is exactly this
#    application: this plugin is loaded into the same process as vsg, which resolves
#    Vulkan entry points BY NAME, so ~400 data symbols of ours named exactly like entry
#    points would be candidates for those lookups — a crash, not a wrong value. With the
#    option the definitions live in `namespace volk` and are therefore mangled symbols
#    (`nm -D` on the built plugin: zero exported symbols named like a Vulkan entry point,
#    and volk's own as `_ZN4volk...`), while volk.h appends `using namespace volk;`, so
#    call sites still read naturally. It requires compiling volk.c as C++, which is what
#    the option does in upstream's own CMakeLists and what the LANGUAGE property below
#    repeats.
#
#  * VOLK_NO_DEVICE_PROTOTYPES — hides the device-level globals, so a device function
#    can ONLY be reached through a `VolkDeviceTable`. This backend loads exactly one such
#    table per session (see VsgVulkanEntryPoints: it needs three extension commands the
#    loader does not export by name), and hiding the globals is what turns "forgot to
#    load the table" from a null-pointer call into a compile error.
#
# The remaining options upstream offers (VOLK_PULL_IN_VULKAN, VOLK_STATIC_DEFINES,
# VOLK_HEADERS_ONLY) are left at their defaults.
add_library(volk STATIC volk.c volk.h)
add_library(tp::volk ALIAS volk)

set_target_properties(volk PROPERTIES FOLDER third_party)

# PIC is load-bearing: volk is linked STATICALLY into the plugin MODULE (gfx_backend_vsgd.so). Without it
# volk.c.o keeps PC32 relocations against its own exported globals, and whether a link tolerates that
# depends on the object's build history and the linker - measured 2026-09-25: the ASan tree's ld.bfd
# refused the link outright ("relocation R_X86_64_PC32 ... can not be used when making a shared object;
# recompile with -fPIC") while the Debug/Release trees were passing with non-PIC objects built earlier.
# PIC here is what removes the luck; a static archive linked into a shared library always wants it.
set_target_properties(volk PROPERTIES POSITION_INDEPENDENT_CODE ON)

# VOLK_NAMESPACE is PUBLIC: it changes what volk.h declares, so volk.c and every consumer have to agree on it.
target_compile_definitions(volk PUBLIC VOLK_NAMESPACE)

# VOLK_NO_DEVICE_PROTOTYPES is INTERFACE-only, and that asymmetry is load-bearing: it hides the device-level
# globals from CONSUMERS (so a device command can only be reached through a VolkDeviceTable — see above),
# while volk.c still has to see them to define and fill them. Making it PUBLIC is what upstream's option name
# suggests and does not compile: volk.c's loader functions assign to those globals.
target_compile_definitions(volk INTERFACE VOLK_NO_DEVICE_PROTOTYPES)

# volk.c must be compiled as C++ for the namespace (see above).
set_source_files_properties(volk.c PROPERTIES LANGUAGE CXX)

target_include_directories(volk PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)

# Vulkan headers: volk.c includes <volk.h>, which includes <vulkan/vk_platform.h>. On Linux those headers sit in the
# default include path, which is why this target used to need nothing; Windows has no such directory - they exist only
# inside the SDK (%VULKAN_SDK%\include) - and volk.c then fails with "cannot open include file:
# vulkan/vk_platform.h" while every other target in the tree gets them through the Vulkan package. Linking the
# interface target FindVulkan provides puts the same directory on this target's include path.
find_package(Vulkan REQUIRED)
target_link_libraries(volk PUBLIC Vulkan::Headers)

# volkInitialize() dlopens the loader itself (libvulkan.so.1 / vulkan-1.dll).
if(NOT WIN32)
    target_link_libraries(volk PUBLIC ${CMAKE_DL_LIBS})
endif()
