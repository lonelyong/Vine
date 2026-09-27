#pragma once

#include <vine/vine_global.hpp>

#ifdef VN_GRAPHICS_LIB
#    define VN_GRAPHICS_API VN_EXPORT
#else
#    define VN_GRAPHICS_API VN_IMPORT
#endif

#define VN_GRAPHICS_NS_BEGIN                                                                                                                                    \
    namespace VN_ROOT_NS                                                                                                                                        \
    {                                                                                                                                                          \
    namespace graphics                                                                                                                                         \
    {

#define VN_GRAPHICS_NS_END                                                                                                                                      \
    }                                                                                                                                                          \
    }

/**
 * @brief The BACKEND half of this module: the headers under `vine/graphics/backend/`.
 *
 * The host half (everything else under `vine/graphics/`) is what an application links; this half is
 * the frame machinery a BACKEND uses to honour RenderBackend - the protocol, the frame description
 * and its compiler, pass order, pipeline keys, target plans, storage, lifetime and the evidence
 * counters. A host must not include it, and this half never includes a particular backend:
 * scripts/check_include_hygiene.py enforces both directions (see .ai/design/graphics-layering.md).
 */
#define VN_GRAPHICSBACKEND_NS_BEGIN \
    VN_GRAPHICS_NS_BEGIN          \
    namespace backend               \
    {

#define VN_GRAPHICSBACKEND_NS_END \
    VN_GRAPHICS_NS_END          \
    }
