#pragma once

#include <span>

#include <vine/meshio/MeshFormat.hpp>

VN_MESHIO_NS_BEGIN
namespace detail
{

/**
 * @brief Returns every format the module declares, in table order.
 *
 * Internal to the module: the public surface states the contract through MeshLoader::supportedExtensions() and
 * formatInfo(), and this exists so those two are generated from ONE table instead of each keeping a list.
 *
 * @return The declared formats; never empty.
 */
std::span<const MeshFormat> allFormats() noexcept;

} // namespace detail
VN_MESHIO_NS_END
