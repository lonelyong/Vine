#include <vine/runtime/KeyValueStore.hpp>

VN_RUNTIME_NS_BEGIN

/**
 * @brief Defines the special members out of line, so the shared library exports them.
 *
 * @details
 * - A `dllimport` class's implicitly defined special members are imported, never emitted locally, so a header-only
 *   defaulted constructor/destructor leaves the client with an unresolved import. The definitions here are built as
 *   part of this module and are therefore what the import library names.
 */
KeyValueStore::KeyValueStore() = default;

KeyValueStore::~KeyValueStore() = default;

VN_RUNTIME_NS_END
