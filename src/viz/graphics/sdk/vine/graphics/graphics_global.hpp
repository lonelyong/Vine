#pragma once

#include <vine/vn_global.hpp>

/**
 * @brief This module's ownership vocabulary: which spelling a signature uses to say who owns what.
 *
 * @details
 * - FOUR SPELLINGS, and a signature that means one of them is written with the one that means it - so a reader
 *   learns a lifetime from the declaration instead of from the implementation:
 *   - `intrusive_ptr<T>` / `intrusive_ptr<const T>`: SHARED OWNERSHIP. It keeps what it names alive, and the
 *     identity is stable enough to hand across threads (a `Geometry`, a `ShaderProgram`, a published snapshot).
 *   - `raw_ptr<T>`: BORROWED. Valid for the call or the documented scope, and a returning function's `@return`
 *     says "borrowed" (a frame's draws borrow their content; a device hands out a window target it owns).
 *   - `std::shared_ptr<const T>`: IMMUTABLE SHARED VALUE. Nobody mutates it after publication, so the holder may
 *     keep it and read it from another thread (a command list, a scene snapshot).
 *   - `std::span<const T>` / `std::span<T>`: NON-OWNING CONTIGUOUS DATA, valid for the call (packed blocks,
 *     vertex data).
 * - A MUTABLE `std::shared_ptr<T>` IN A PUBLIC SIGNATURE is a boundary that is missing, not a fifth spelling:
 *   two holders of a shared AND mutable value can disagree, and no caller can say what it is looking at. Inside an
 *   implementation (a cache table, a PImpl block) it is ordinary bookkeeping under that unit's own lock.
 * - THE HOST PAYS FOR THIS BEING WRITTEN DOWN: a frame value (`FrameDescription`) may be retained only as its own
 *   type says, and a function whose `@return` is silent about "borrowed" or "owned" is a documentation defect.
 */
#ifdef VN_GRAPHICS_LIB
#    define VN_GRAPHICS_API VN_EXPORT
#else
#    define VN_GRAPHICS_API VN_IMPORT
#endif

#define VN_GRAPHICS_NS_BEGIN                                                                                                                                   \
    namespace VN_ROOT_NS                                                                                                                                       \
    {                                                                                                                                                          \
    namespace graphics                                                                                                                                         \
    {

#define VN_GRAPHICS_NS_END                                                                                                                                     \
    }                                                                                                                                                          \
    }

/**
 * @brief The BACKEND half of this module: the headers under `vine/graphics/backend/`.
 *
 * @details
 * - The host half (everything else under `vine/graphics/`) is what an application links; this half is
 *   the frame machinery a BACKEND uses to honour RenderBackend - the protocol, the frame description
 *   and its compiler, pass order, pipeline keys, target plans, storage, lifetime and the evidence
 *   counters.
 * - A host must not include it, and this half never includes a particular backend:
 *   scripts/check_include_hygiene.py enforces both directions (see .ai/design/graphics-architecture.md).
 */
#define VN_GRAPHICSBACKEND_NS_BEGIN                                                                                                                            \
    VN_GRAPHICS_NS_BEGIN                                                                                                                                       \
    namespace backend                                                                                                                                          \
    {

#define VN_GRAPHICSBACKEND_NS_END                                                                                                                              \
    VN_GRAPHICS_NS_END                                                                                                                                         \
    }
