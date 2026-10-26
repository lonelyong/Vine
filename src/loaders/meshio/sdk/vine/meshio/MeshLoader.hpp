#pragma once

#include "meshio_global.hpp"

#include <cstddef>
#include <filesystem>
#include <istream>
#include <set>
#include <vector>

#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/crypto/ByteSequenceFingerprint.hpp>
#include <vine/geometry/Mesh.hpp>
#include <vine/geometry/Model.hpp>
#include <vine/io/IoError.hpp>
#include <vine/runtime/InMemoryCache.hpp>

VN_MESHIO_NS_BEGIN

/**
 * @brief Utility class for loading mesh models (STL, OBJ, ...).
 *
 * The two entries are named after what they hand back: load() keeps what the source described (node tree, meshes,
 * materials), loadMerged() flattens the same file into one mesh. Both results are cached by content fingerprint, so
 * loading the same file again reuses what was built before; the options belong to the INSTANCE, so two scaling policies
 * are two loaders rather than one cache to be keyed twice.
 *
 * @note The cache is not thread-safe; concurrent loads through the same
 *       loader instance must be avoided.
 */
class VN_MESHIO_API MeshLoader
{
    // 类型声明区块
  public:
    /**
     * @brief Length unit a model's coordinates are expressed in.
     *
     * The enumerator's VALUE is how many of that unit make one metre, which is the ratio the Auto rule divides: a
     * source in metres scaled to millimetres multiplies by 1000 / 1. Units that do not divide a metre evenly cannot be
     * spelled this way.
     */
    enum class LengthUnit
    {
        /// Metre, one per metre.
        Meter = 1,
        /// Millimetre, a thousand per metre.
        Millimeter = 1000,
    };

    /** @brief Vertex scaling strategy. */
    enum class ScaleMode
    {
        /// No scaling is applied.
        Disabled,
        /// The source unit is inferred from the model's AABB diagonal length.
        Auto,
        /// A custom factor scales the vertices.
        Custom,
    };

    /** @brief Load options. */
    struct Options
    {
        /**
         * @brief Vertex scaling mode.
         *
         * @details
         * - Auto, the default, INFERS the source unit from the model itself: an AABB diagonal longer than 10 source
         *   units is read as millimetres, anything shorter as metres, and the vertices are then scaled to
         *   auto_scale_output_unit - so a model a couple of metres across is multiplied by 1000. Say what the source
         *   is (Disabled or Custom) when it is known.
         */
        ScaleMode scale_mode{ ScaleMode::Auto };

        /// Target unit used when Auto scaling is active.
        LengthUnit auto_scale_output_unit{ LengthUnit::Millimeter };

        /// Custom scale factor; only used in Custom mode.
        double custom_scale_factor{ 1.0 };

        /**
         * @brief Compares two option sets for equality.
         *
         * @param rhs The option set to compare with.
         * @return true when every field matches.
         */
        bool operator==(const Options& rhs) const noexcept
        {
            return scale_mode == rhs.scale_mode && auto_scale_output_unit == rhs.auto_scale_output_unit
                   && custom_scale_factor == rhs.custom_scale_factor;
        }
    };

    // 构造函数区块
  public:
    /**
     * @brief Creates a loader with @p options.
     *
     * @details
     * - The options and the cache belong to THIS loader: two callers that need different scaling policies each keep
     *   their own instance instead of taking turns on one. defaultInstance() is the convenience instance for a caller
     *   that wants the defaults and does not mind sharing.
     * - Everything a load built stays alive in this loader's cache until clearCache(), so an instance that loads
     *   content all day needs to be told when the content it caches is no longer wanted.
     *
     * @param options The load options; the default is Auto scaling to millimetres (see Options).
     */
    explicit MeshLoader(Options options = {});
    MeshLoader(const MeshLoader&) = delete;
    MeshLoader(MeshLoader&&) = delete;
    ~MeshLoader();

    // 方法区块
  public:
    /**
     * @brief Returns the shared default loader instance.
     *
     * @return The singleton instance.
     */
    static MeshLoader& defaultInstance();

    /**
     * @brief Checks whether a file is a supported mesh format.
     *
     * @param file_path The model file path.
     * @return true when the file extension is supported.
     */
    static bool isSupportedFormat(const std::filesystem::path& file_path);

    /**
     * @brief Returns the file extensions this loader can read.
     *
     * The single source of truth for "what can be opened": a file picker builds its filter out of this instead of keeping
     * a second copy that drifts as soon as a format is added. Extensions are lowercase and carry the leading dot.
     *
     * @return The supported extension set, e.g. {".stl", ".obj", ...}.
     */
    static const std::set<vn::String>& supportedExtensions();

    /**
     * @brief Returns the load options.
     *
     * @return The options this loader was constructed with.
     */
    [[nodiscard]] const Options& options() const noexcept;

    /**
     * @brief Drops every cached mesh and model.
     *
     * @details
     * - The cache keeps what it built alive until it is told otherwise, so a caller that loads content all day calls
     *   this when the files it holds are no longer wanted.
     * - What callers already hold stays valid: this releases the cache's share, not theirs.
     */
    void clearCache();

    /**
     * @brief Loads a whole model from a file, keeping its structure.
     *
     * This is the entry that keeps what the source described: the node tree with its transforms, one entry per mesh,
     * and the material each mesh uses. The meshes stay in their own local space (nothing is baked), and texture
     * coordinates are kept, because a caller that asked for the structure wants what the file held.
     *
     * @details
     * - It is the entry to use when the file refers to OTHER files: assimp resolves them relative to @p file_path (an
     *   OBJ and the .mtl beside it), which a memory image cannot do.
     * - Like loadMerged(), the result is cached by CONTENT, so an edited file is parsed again and an unchanged one is
     *   not. Reading it once to fingerprint it and once to parse it is the price of not serving stale geometry.
     *
     * @param file_path The model file path (STL, OBJ, glTF, ...).
     * @param error     Receives the failure class when the call returns null; may be null when the caller does not care.
     * @return The model, or null on failure (see @p error).
     */
    vn::intrusive_ptr<vn::geometry::Model> load(const std::filesystem::path& file_path,
                                               vn::io::IoError*         error = nullptr);

    /**
     * @brief Loads a whole model from a stream, keeping its structure.
     *
     * The stream is drained first, exactly like loadMerged(): the loader parses a memory image rather than a stream,
     * so this is the entry that reads a model out of a package without touching the file system.
     *
     * @param in          The stream to read; it is read to its end.
     * @param format_hint The format to parse as: one of supportedExtensions(), with or without the leading dot (e.g.
     *                    "stl" or ".stl"). An empty or null hint lets assimp guess from the content, which it can only
     *                    do for a format it can recognise without a name; state it when the source knows it.
     * @param error       Receives the failure class when the call returns null; may be null when the caller does not
     *                    care.
     * @return The model, or null on failure (see @p error).
     */
    vn::intrusive_ptr<vn::geometry::Model> load(std::istream& in, const char* format_hint,
                                               vn::io::IoError* error = nullptr);

    /**
     * @brief Loads a mesh model from a file as one flattened mesh.
     *
     * Every mesh the source holds is concatenated into one mesh with the node transforms applied; materials and the
     * node tree are dropped, so a caller that needs either wants load() instead.
     *
     * @details
     * - ATTRIBUTES follow the meshes: vertex colours are kept when at least one mesh authors them, and the vertices of
     *   the meshes that author none carry opaque white - the multiplicative identity the shading applies - so each part
     *   looks the way it would on its own. Normals the source only partly provides are dropped whole and derived from the
     *   geometry instead, because a zero normal is not "no normal". Texture coordinates are not kept by this entry.
     * - The result is an IndexedTriangleMesh: every format this loader reads is triangulated on the way in, so there is
     *   one concrete type behind the Mesh handle.
     *
     * @param file_path The model file path (STL, OBJ, ...).
     * @param error     Receives the failure class when the call returns null; may be null when the caller does not care.
     * @return The loaded mesh, or null on failure (see @p error).
     */
    vn::intrusive_ptr<vn::geometry::Mesh> loadMerged(const std::filesystem::path& file_path,
                                                     vn::io::IoError*         error = nullptr);

    /**
     * @brief Loads a mesh model from a stream as one flattened mesh.
     *
     * The loader parses a memory image rather than a stream, so the stream is drained first: this entry trades the
     * temporary file for one copy of the model in memory, and it is how a model that lives in a package is loaded
     * without touching the file system at all (wrap a VfsEntrySource in a vn::io::DataSourceStream to read one).
     *
     * @note The cache is keyed by the bytes and the options, not by @p format_hint: the same bytes parsed under a
     *       different hint after a successful load answer the first result. A load that failed caches nothing.
     *
     * @param in          The stream to read; it is read to its end.
     * @param format_hint The format to parse as: one of supportedExtensions(), with or without the leading dot (e.g.
     *                    "stl" or ".stl"). An empty or null hint lets assimp guess from the content, which it can only
     *                    do for a format it can recognise without a name; state it when the source knows it.
     * @param error       Receives the failure class when the call returns null; may be null when the caller does not
     *                    care.
     * @return The loaded mesh, or null on failure (see @p error).
     */
    vn::intrusive_ptr<vn::geometry::Mesh> loadMerged(std::istream& in, const char* format_hint,
                                                    vn::io::IoError* error = nullptr);

    // 方法区块
  private:
    /**
     * @brief Parses a whole model from the bytes of a model file, sharing the cache with the path entry.
     *
     * The counterpart of the path entry: what that one reads from a file, this one is handed as the bytes the file was
     * read into, so a model that lives in a package is parsed the same way as one that lives on disk.
     *
     * @param bytes The bytes of the model; they stay alive for the duration of the call.
     * @param format_hint The format to parse as.
     * @param error Receives the failure class when the call returns null.
     * @return The model, or null on failure (see @p error).
     */
    vn::intrusive_ptr<vn::geometry::Model> loadBytes(std::vector<unsigned char>& bytes, const char* format_hint,
                                                     vn::io::IoError* error);

    /**
     * @brief Parses the bytes of a model into one flattened mesh, sharing the cache with the path entry.
     *
     * @param bytes The bytes of the model; they stay alive for the duration of the call.
     * @param format_hint The format to parse as.
     * @param error Receives the failure class when the call returns null.
     * @return The loaded mesh, or null on failure (see @p error).
     */
    vn::intrusive_ptr<vn::geometry::Mesh> loadMergedBytes(std::vector<unsigned char>& bytes, const char* format_hint,
                                                         vn::io::IoError* error);

    /**
     * @brief Caches a built model under its content fingerprint and hands it to the caller.
     *
     * @param fingerprint The content fingerprint; an empty one is not cached.
     * @param model The model to cache; it is returned unchanged.
     * @return The model the caller receives.
     */
    vn::intrusive_ptr<vn::geometry::Model> cacheModel(const vn::crypto::ByteSequenceFingerprint& fingerprint,
                                                      vn::intrusive_ptr<vn::geometry::Model>        model);

    // 类型声明区块
  private:
    /** @brief What the cache holds for one file: whichever of the two results has been built from it. */
    struct Cached
    {
        vn::intrusive_ptr<vn::geometry::Mesh>  merged; ///< The flattened result, once loadMerged() built it.
        vn::intrusive_ptr<vn::geometry::Model> model;  ///< The structured result, once load() built it.
    };

    // 字段区块
  private:
    Options options_;
    vn::runtime::InMemoryCache<vn::crypto::ByteSequenceFingerprint, Cached> cache_;
};

VN_MESHIO_NS_END
