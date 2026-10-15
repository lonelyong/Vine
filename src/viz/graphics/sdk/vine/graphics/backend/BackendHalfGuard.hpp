#pragma once

/**
 * @brief Makes "the backend half is a backend's business" a BUILD error, not only a gate finding.
 *
 * @details
 * - A header under `vine/graphics/backend/` is the frame machinery a BACKEND uses (see
 *   graphics_global.hpp's VN_GRAPHICSBACKEND_NS_BEGIN, and .ai/design/graphics-layering.md). A host
 *   renders through `vine/graphics/*.hpp` only, but reaching into this half still compiles - so the
 *   rule used to hold only in scripts/check_include_hygiene.py, at gate time.
 * - Including this header makes that rule a compile error: a translation unit that reaches the backend
 *   half must FIRST define VN_GRAPHICS_BACKEND_CONSUMER. A render backend (`gfx_backend_*`), the
 *   backend's own test suite (`test_vsg`) and libGraphics itself (which compiles the half) define it;
 *   a host target does not, so a stray include fails at build time instead of at review time.
 *
 * @note Every header of this half includes this one FIRST, so the check cannot be bypassed by reaching
 *       a second backend header instead.
 */

#ifndef VN_GRAPHICS_BACKEND_CONSUMER
#    error "vine/graphics/backend/ is the BACKEND half of graphics: a host includes only \
vine/graphics/*.hpp. Define VN_GRAPHICS_BACKEND_CONSUMER to opt in (a render backend and the \
backend's own tests do; see .ai/design/graphics-layering.md)."
#endif
