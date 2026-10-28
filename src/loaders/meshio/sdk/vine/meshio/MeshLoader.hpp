#pragma once

#include "meshio_global.hpp"

#include <cstddef>
#include <filesystem>
#include <istream>
#include <set>
#include <vector>

#include <vine/String.hpp>
#include <vine/crypto/ByteSequenceFingerprint.hpp>
#include <vine/geometry/Mesh.hpp>
#include <vine/geometry/Model.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/io/IoError.hpp>
#include <vine/runtime/InMemoryCache.hpp>

#include "MeshFormat.hpp"

VN_MESHIO_NS_BEGIN

/**
 * @brief Loads mesh models (STL, OBJ, glTF, ...) as a structure or as one merged mesh.
 *
 * @details
 * - The two entries are named after what they hand back: load() keeps what the source described (node tree, meshes,
 *   materials), loadMerged() flattens the same file into one mesh.
 * - Both answer a vn::io::Result: the geometry on success, the failure class otherwise. A path whose extension the
 *   format table does not name is IoError::Unsupported answered before anything is read, so a file picker built on
 *   supportedExtensions() can never offer something the loader would refuse.
 * - Results are cached by content fingerprint, so loading the same file again reuses what was built before. The
 *   options belong to the INSTANCE, so two scaling policies are two loaders rather than one cache keyed twice.
 *
 * @warning
 * - The cache is not thread-safe: one loader instance must not be used from two threads at the same time.
 * - A long-lived loader keeps every result it built until clearCache() is called.
 */
class VN_MESHIO_API MeshLoader {
  public:
    /**
     * @brief Length unit a model's coordinates are expressed in.
     *
     * The enumerator's VALUE is how many of that unit make one metre, which is the ratio the Auto rule divides: a
     * source in metres scaled to millimetres multiplies by 1000 / 1.
     */
    enum class LengthUnit
    {
        /// Metre, one per metre.
        Meter      = 1,
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
    struct Options {
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
            return scale_mode == rhs.scale_mode && auto_scale_output_unit == rhs.auto_scale_output_unit && custom_scale_factor == rhs.custom_scale_factor;
        }
    };

  public:
    /**
     * @brief Creates a loader with the default options.
     *
     * @details
     * - Equivalent to `MeshLoader(Options{})`: Auto scaling to millimetres (see Options).
     * - Declared separately because a default argument cannot be written for the nested Options type here: its
     *   default member initializers are not available yet while the enclosing class is still being defined.
     */
    MeshLoader();

    /**
     * @brief Creates a loader with @p options.
     *
     * @details
     * - The options and the cache belong to THIS loader: two callers that need different scaling policies each keep
     *   their own instance instead of taking turns on one. There is no shared default instance, because its options
     *   would be shared mutable state and its cache is not thread-safe.
     *
     * @param options The load options.
     */
    explicit MeshLoader(Options options);
    MeshLoader(const MeshLoader&) = delete;
    MeshLoader(MeshLoader&&)      = delete;
    ~MeshLoader();

  public:
    /**
     * @brief Returns the file extensions this loader can read.
     *
     * The single source of truth for "what can be opened": a file picker builds its filter out of this instead of
     * keeping a second copy that drifts as soon as a format is added. The set is generated from the format table, so
     * it names exactly what formatFromPath() recognises. Extensions are lowercase and carry the leading dot.
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
     * - It is the entry to use when the file refers to OTHER files: they are resolved relative to @p file_path (an OBJ
     *   and the .mtl beside it), which a memory image cannot do.
     * - The result is cached by CONTENT, so an edited file is parsed again and an unchanged one is not.
     *
     * @param file_path The model file path; its extension selects the format.
     * @return The model, or the failure class: Unsupported when the extension names no format, NotFound when the file
     *         is not there, IoFailure when it cannot be read, InvalidData when it cannot be parsed.
     */
    [[nodiscard]] vn::io::Result<vn::intrusive_ptr<vn::geometry::Model>> load(const std::filesystem::path& file_path);

    /**
     * @brief Loads a whole model from a stream, keeping its structure.
     *
     * The stream is drained first: the loader parses a memory image rather than a stream, so this is the entry that
     * reads a model out of a package without touching the file system. The bytes are cached, which means a second call
     * with the same content answers the object the first one built.
     *
     * @note The bytes are keyed by content, not by @p hint: the same content parsed under a different hint after a
     *       successful load answers the first result.
     *
     * @param in   The stream to read; it is read to its end.
     * @param hint The format to parse as, because a stream carries no file name to infer it from. MeshFormat::Unknown
     *             lets the parser guess from the content, which only works for a format it can recognise without a
     *             name; state it when the source knows it.
     * @return The model, or the failure class: IoFailure when the stream breaks, InvalidData when the bytes cannot be
     *         parsed.
     */
    [[nodiscard]] vn::io::Result<vn::intrusive_ptr<vn::geometry::Model>> load(std::istream& in, MeshFormat hint = MeshFormat::Unknown);

    /**
     * @brief Loads a mesh model from a file as one flattened mesh.
     *
     * Every mesh the source holds is concatenated into one mesh with the node transforms applied; materials and the
     * node tree are dropped, so a caller that needs either wants load() instead.
     *
     * @details
     * - ATTRIBUTES follow the meshes: vertex colours are kept when at least one mesh authors them, and the vertices of
     *   the meshes that author none carry opaque white - the multiplicative identity the shading applies - so each part
     *   looks the way it would on its own. Normals the source only partly provides are dropped whole and derived from
     *   the geometry instead, because a zero normal is not "no normal". Texture coordinates are not kept by this entry.
     * - The result is an IndexedTriangleMesh: every format this loader reads is triangulated on the way in, so there is
     *   one concrete type behind the Mesh handle.
     *
     * @param file_path The model file path; its extension selects the format.
     * @return The loaded mesh, or the failure class (see the model entry).
     */
    [[nodiscard]] vn::io::Result<vn::intrusive_ptr<vn::geometry::Mesh>> loadMerged(const std::filesystem::path& file_path);

    /**
     * @brief Loads a mesh model from a stream as one flattened mesh.
     *
     * The stream entry of loadMerged(): the stream is drained, the bytes are cached, and the result is one
     * IndexedTriangleMesh with the node transforms applied.
     *
     * @param in   The stream to read; it is read to its end.
     * @param hint The format to parse as (see the model entry); required for a format the parser cannot guess.
     * @return The loaded mesh, or the failure class (see the model entry).
     */
    [[nodiscard]] vn::io::Result<vn::intrusive_ptr<vn::geometry::Mesh>> loadMerged(std::istream& in, MeshFormat hint = MeshFormat::Unknown);

  private:
    /**
     * @brief Parses a model out of bytes already in memory, sharing the cache with the path entry.
     *
     * @param bytes The bytes of the model; they stay alive for the duration of the call.
     * @param hint The format to parse as; Unknown lets the parser guess.
     * @return The model, or the failure class.
     */
    vn::io::Result<vn::intrusive_ptr<vn::geometry::Model>> loadBytes(std::vector<unsigned char>& bytes, MeshFormat hint);

    /**
     * @brief Parses a flattened mesh out of bytes already in memory, sharing the cache with the path entry.
     *
     * @param bytes The bytes of the model; they stay alive for the duration of the call.
     * @param hint The format to parse as; Unknown lets the parser guess.
     * @return The loaded mesh, or the failure class.
     */
    vn::io::Result<vn::intrusive_ptr<vn::geometry::Mesh>> loadMergedBytes(std::vector<unsigned char>& bytes, MeshFormat hint);

    /**
     * @brief Caches a built model under its content fingerprint and hands it to the caller.
     *
     * @param fingerprint The content fingerprint; an empty one is not cached.
     * @param model The model to cache; it is returned unchanged.
     * @return The model the caller receives.
     */
    vn::intrusive_ptr<vn::geometry::Model> cacheModel(const vn::crypto::ByteSequenceFingerprint& fingerprint, vn::intrusive_ptr<vn::geometry::Model> model);

    /** @brief What the cache holds for one file: whichever of the two results has been built from it. */
    struct Cached {
        vn::intrusive_ptr<vn::geometry::Mesh>  merged; // The flattened result, once loadMerged() built it.
        vn::intrusive_ptr<vn::geometry::Model> model;  // The structured result, once load() built it.
    };

    Options                                                                 options_;
    vn::runtime::InMemoryCache<vn::crypto::ByteSequenceFingerprint, Cached> cache_;
};

VN_MESHIO_NS_END
