#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include <vine/geom/spatial/BvhTopology.hpp>
#include <vine/io/Vfs.hpp>
#include <vine/models/Mesh.hpp>
#include <vine/robotics/io/robot_io_global.hpp>

#include "BvhCacheFormat.hpp"

VN_ROBOTICS_IO_NS_BEGIN

namespace detail
{

/**
 * @brief Returns the cache file that belongs beside a mesh file.
 *
 * @details
 * - The name is derived from the mesh file's, so a package's description and manifest say nothing about the cache: it is
 *   a file that may be missing, deleted or stale, and the mesh still loads exactly as it would have.
 *
 * @param mesh_path The mesh file inside the package ("geoms/mesh0.vmesh", "geoms/mesh0.stl", ...).
 * @return The cache path for it ("geoms/mesh0.vbtp").
 */
[[nodiscard]] inline std::filesystem::path bvhCachePath(const std::filesystem::path& mesh_path)
{
    std::filesystem::path cache_path = mesh_path;
    cache_path.replace_extension(".vbtp");
    return cache_path;
}

/**
 * @brief Writes a mesh's prepared tree as the cache file beside it.
 *
 * @details
 * - The tree's SHAPE is what a cache stores (see `encodeBvhCache`, next door): taken from the mesh's PUBLISHED tree, so
 *   the cache always describes the tree the process is actually using, and written through the same view a decoder hands
 *   back - one representation, no conversion in between.
 * - A mesh with no prepared tree writes nothing - the caller decides whether building one for export is worth it - and a
 *   tree that does not cover the mesh's triangles is not this mesh's cache, so it is not written either.
 *
 * @param vfs The tree to write into.
 * @param mesh_path The mesh file the cache belongs to; the cache path is derived from it.
 * @param mesh The mesh whose tree is stored.
 * @return true when the cache file was written.
 */
inline bool writeBvhCache(vn::io::Vfs& vfs, const std::filesystem::path& mesh_path, const vn::models::Mesh& mesh)
{
    if (!mesh.hasBvh()) {
        return false;
    }
    // The tree's own arrays, borrowed: saving a cache walks what the tree already holds instead of transcribing it first.
    const vn::geom::spatial::BvhTopologyView topology = mesh.bvh().topologyView();
    if (topology.primitiveCount() != mesh.triangleCount()) {
        return false; // a tree that does not cover this mesh's triangles is not this mesh's cache
    }

    std::vector<unsigned char> bytes;
    if (!encodeBvhCache(topology, bytes)) {
        return false;
    }
    return vfs.addFile(bvhCachePath(mesh_path), std::span<const unsigned char>(bytes.data(), bytes.size())) == vn::io::IoError::Ok;
}

/**
 * @brief Reads a cache file into the shape it stores.
 *
 * @details
 * - Every refusal is the same answer: `false`. A missing file is the ordinary case, not an error; a damaged one is
 *   treated the same way, because a cache is an optional acceleration structure and rebuilding one costs time, not data.
 * - The numbers are NOT compared with any triangle set here: this function does not know one. `restoreBvhCache()` is
 *   the call that does, and `Mesh::restoreBvh()` is where a shape for another mesh is refused.
 *
 * @param vfs The tree to read from.
 * @param cache_path The cache file.
 * @param out Receives the decoded shape; untouched when this returns false.
 * @return true when the file was a cache of a version this build reads and held all of it.
 */
inline bool readBvhCache(vn::io::Vfs& vfs, const std::filesystem::path& cache_path, vn::geom::spatial::BvhTopology& out)
{
    auto bytes = vfs.read(cache_path);
    if (!bytes) {
        return false; // no cache is the ordinary case, not an error
    }
    return decodeBvhCache(std::span<const unsigned char>(bytes->data(), bytes->size()), out);
}

/**
 * @brief Restores a mesh's tree from the cache beside its mesh file, if that cache describes THIS mesh.
 *
 * @details
 * - Every refusal is the same answer: "no cache". A missing file, a foreign path, a damaged one, or a shape that does
 *   not describe the loaded mesh's triangles all leave the mesh without a tree, which costs a build and nothing else.
 * - This is the whole read side of the persistence contract: the mesh is what the file says it is, and the tree is an
 *   optional acceleration structure beside it. The decoded shape is MOVED into the tree, so nothing is copied between
 *   the decoder's arrays and the tree's own.
 *
 * @param vfs The tree to read from.
 * @param mesh_path The mesh file that was just loaded.
 * @param mesh The loaded mesh; receives the tree.
 * @return true when the mesh ended up with a restored tree.
 */
inline bool restoreBvhCache(vn::io::Vfs& vfs, const std::filesystem::path& mesh_path, vn::models::Mesh& mesh)
{
    vn::geom::spatial::BvhTopology topology;
    if (!readBvhCache(vfs, bvhCachePath(mesh_path), topology)) {
        return false;
    }
    return mesh.restoreBvh(std::move(topology));
}

} // namespace detail

VN_ROBOTICS_IO_NS_END
