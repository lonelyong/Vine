#pragma once

#include <span>

#include <vine/brepio/BrepFormat.hpp>

VN_BREPIO_NS_BEGIN
namespace detail
{

/**
 * @brief Returns every format the module declares, in table order.
 *
 * Internal to the module: MeshLoader-like callers state the contract through BrepLoader::supportedExtensions() and
 * formatFromPath(), and this exists so those two are generated from ONE table.
 *
 * @return The declared formats; never empty.
 */
std::span<const BrepFormat> allFormats() noexcept;

/**
 * @brief Returns the extensions one format is named by.
 *
 * A format may have more than one spelling (".stp" and ".step"), so a caller that builds an extension list asks here
 * instead of keeping a second copy of the table.
 *
 * @param format Format to look up.
 * @return The extensions, without trailing nulls; empty for Unknown.
 */
std::span<const char* const> extensionsOf(BrepFormat format) noexcept;

} // namespace detail
VN_BREPIO_NS_END
