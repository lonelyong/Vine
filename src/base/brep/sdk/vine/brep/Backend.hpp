#pragma once

#include "brep_global.hpp"

VN_BREP_NS_BEGIN

/**
 * @brief Reports whether this build links a B-rep backend.
 *
 * @details
 * - This is a build property, not a per-call outcome: every operation of this module answers it the same way every
 *   time, so one query serves them all.
 * - It is what tells a build-level cause apart from an input-level one when an operation returns a null result:
 *   "this build has no backend" versus "the solid holds no shape" or "the primitive is invalid".
 * - The capability belongs to the module, not to one operation: tessellation and construction are compiled from the
 *   same backend, so a query named after either of them would make the other operation's callers ask the wrong
 *   question. A future backend that supports only one of them is the point at which this splits (see
 *   `.ai/backlog/brep.md`).
 *
 * @note
 * - The backend is compiled in only when it is found (`VN_BREP_HAS_OCCT`), and that definition stays private to this
 *   module, so the answer cannot be a constant in the header. A build without the backend keeps every entry point
 *   and answers false here; that branch still needs a build with the backend to be verified (see
 *   `.ai/backlog/brep.md`).
 *
 * @return True when a backend is built into this module.
 */
[[nodiscard]] VN_BREP_API bool hasBackend() noexcept;

VN_BREP_NS_END
