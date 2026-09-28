#pragma once

#include "meshio_global.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <vine/intrusive_ptr.hpp>
#include <vine/geometry/Mesh.hpp>
#include <vine/io/Stream.hpp>

VN_MESHIO_NS_BEGIN

/**
 * @brief The half of a mesh-to-bytes source that every format shares.
 *
 * The lazy counterpart of MeshExporter: nothing is converted when a source is made, and the bytes are produced as they
 * are pulled - so a mesh reaches a package (or any other target) without a second, whole copy of the model ever
 * existing. The source holds the mesh, so it stays valid without the caller tracking a lifetime.
 *
 * A source hands out RECORDS: the smallest unit its format can produce - a binary STL triangle, an ASCII STL facet, an
 * OBJ line. Every record is a function of its index alone, which is what makes a pull resumable: a caller whose buffer
 * cuts a record in half gets the rest of it from the same record, rebuilt on the spot, so there is no half-finished
 * state to carry and no whole image to hold.
 *
 * A concrete format is one of BinStlSource, AsciiStlSource and ObjSource; it says how many records it has, how large
 * the largest one can get, and how to render one. Everything else - owning the mesh, watching it for edits, the pull
 * cursor, the scratch record - lives here.
 *
 * THE LENGTH MAY BE UNKNOWN. A fixed-layout format states its length (binary STL knows it before the first byte); a
 * text format only knows it once the text is laid out, so it reports kUnknownSize and ends when its records run out.
 * Both are acceptable to a backend that can write without the length first (a ZIP stores such an entry with a zip64
 * header whose length is patched in afterwards).
 *
 * THE MESH IS READ LIVE. A source reads the mesh's own buffers, so an edit that changes what it would write (growing or
 * replacing a buffer) is reported as IoError::InvalidData instead of producing content that is half old and half new.
 *
 * @note A mesh the consumer must keep unchanged until the pull is finished is exactly the contract every borrowed
 *       source carries; this one turns a violation into a failed pull rather than into wrong bytes.
 *
 * Threading: no method is thread-safe and there is no internal locking - a shared object, and storage that two objects share, are synchronized by the caller;
 * objects that share nothing (buffer, handle or storage) may be used concurrently.
 */
class VN_MESHIO_API MeshSource : public vn::io::DataSource
{
  public:
    ~MeshSource() override;

    MeshSource(const MeshSource&) = delete;
    MeshSource& operator=(const MeshSource&) = delete;

    /**
     * @brief Reports the length of the content.
     *
     * @return The length the format stated, kUnknownSize when it cannot state one, or 0 when the mesh was refused
     *         (see error()).
     */
    [[nodiscard]] std::uint64_t size() const noexcept override;

    /**
     * @brief Restarts the content from its first record.
     *
     * Nothing accumulates across a pull, so a second pull produces the same bytes.
     */
    void rewind() override;

    /**
     * @brief Writes the next chunk of the content.
     *
     * @param out Buffer to fill; it may be smaller than one record, in which case the remainder is written by the next
     *        call.
     * @return The number of bytes written, or 0 at the end of the content (see error()).
     */
    [[nodiscard]] std::size_t read(std::span<std::byte> out) override;

    /**
     * @brief Reports a failure that read() cannot express.
     *
     * @return IoError::Ok while the mesh writes cleanly, IoError::InvalidData when it was refused (null, invalid,
     *         unsupported shape, no triangles, or an index that does not fit its position array) or when it was edited
     *         after the source was handed over.
     */
    [[nodiscard]] vn::io::IoError error() const override;

  protected:
    /**
     * @brief Snapshots a mesh for writing.
     *
     * The shape is checked here: anything that is not an indexed triangle mesh or a triangle soup is refused, and so is
     * a mesh without triangles (there would be no content to write). A refusal is reported through error() rather than
     * by throwing, so the caller sees it where every other source defect surfaces - addFile() asks for it up front,
     * which turns a broken source into a failed call instead of a failed save.
     *
     * @param mesh The mesh to write; the source holds it.
     * @param scale Factor applied to every vertex, matching what MeshExporter's scale_factor does.
     */
    MeshSource(vn::intrusive_ptr<const vn::geometry::Mesh> mesh, double scale);

    /**
     * @brief Moves a source.
     *
     * A source is handed to a container by value in callers that build it in one expression (a shared_ptr holds it for
     * the lifetime of the content), so moving has to work; copying does not, since the pull cursor is state.
     *
     * @param other The source to move from.
     */
    MeshSource(MeshSource&& other) noexcept;

    /**
     * @brief Takes over another source's state.
     *
     * @param other The source to move from.
     * @return A reference to this source.
     */
    MeshSource& operator=(MeshSource&& other) noexcept;

    /**
     * @brief Returns whether the mesh was accepted.
     *
     * @return true when records can be produced.
     */
    [[nodiscard]] bool ready() const noexcept;

    /**
     * @brief Refuses the mesh, for a format that cannot write what it was given.
     *
     * @note Callable from a constructor, which is the only place a format has to use it.
     */
    void refuse() noexcept;

    /**
     * @brief States the length of the content, for a format that can work it out.
     *
     * A format that calls this is taken at its word: size() answers from it, and a pull that ends at another byte count
     * is reported as IoError::InvalidData rather than handed over as content. A format that does not call it stays at
     * kUnknownSize and ends when its records run out.
     *
     * @param bytes The exact number of bytes the format produces.
     */
    void stateSize(std::uint64_t bytes) noexcept;

    /**
     * @brief Reports how many triangles the mesh holds.
     *
     * @return The triangle count, 0 when the mesh was refused.
     */
    [[nodiscard]] std::uint64_t triangleCount() const noexcept;

    /**
     * @brief Reports how many vertices the mesh positions hold.
     *
     * @return The vertex count, 0 when the mesh was refused.
     */
    [[nodiscard]] std::uint64_t vertexCount() const noexcept;

    /**
     * @brief Reports whether the mesh carries per-vertex normals.
     *
     * @return true when there is one normal per position.
     */
    [[nodiscard]] bool hasNormals() const noexcept;

    /**
     * @brief Reports the factor every vertex is multiplied by.
     *
     * @return The scale factor.
     */
    [[nodiscard]] float scale() const noexcept;

    /**
     * @brief Reads one vertex position, scaled.
     *
     * @param vertex Vertex index.
     * @param out Receives the three coordinates.
     * @return true on success, false when the mesh does not hold that vertex.
     */
    [[nodiscard]] bool vertexAt(std::uint64_t vertex, float (&out)[3]) const;

    /**
     * @brief Reads one vertex normal.
     *
     * @param vertex Vertex index.
     * @param out Receives the three components.
     * @return true on success, false when the mesh carries no normal for that vertex.
     */
    [[nodiscard]] bool normalAt(std::uint64_t vertex, float (&out)[3]) const;

    /**
     * @brief Reads the mesh vertex indices of one triangle.
     *
     * A triangle soup answers with the three consecutive positions the triangle is made of, so callers never have to
     * care which of the two shapes the mesh has.
     *
     * @param triangle Triangle index.
     * @param out Receives the three vertex indices.
     * @return true on success, false when the mesh does not hold that triangle.
     */
    [[nodiscard]] bool cornerAt(std::uint64_t triangle, std::uint32_t (&out)[3]) const;

    /**
     * @brief Reads the three corner positions of one triangle, scaled.
     *
     * @param triangle Triangle index.
     * @param a Receives the first corner.
     * @param b Receives the second corner.
     * @param c Receives the third corner.
     * @return true on success, false when the mesh does not hold that triangle.
     */
    [[nodiscard]] bool triangleAt(std::uint64_t triangle, float (&a)[3], float (&b)[3], float (&c)[3]) const;

    /**
     * @brief Computes the unit normal of a triangle from its winding.
     *
     * STL carries one normal per facet rather than per vertex, so it is computed; a degenerate triangle gets a zero
     * normal, which is what every STL writer does with one.
     *
     * @param a First corner.
     * @param b Second corner.
     * @param c Third corner.
     * @param out Receives the unit normal.
     */
    static void facetNormal(const float (&a)[3], const float (&b)[3], const float (&c)[3], float (&out)[3]) noexcept;

    /**
     * @brief Reports how many records this format hands out.
     *
     * @return The record count; it must not change between pulls.
     */
    [[nodiscard]] virtual std::uint64_t recordCount() const = 0;

    /**
     * @brief Reports how much space one record can need.
     *
     * @return The largest size renderRecord() can return.
     */
    [[nodiscard]] virtual std::size_t maxRecordSize() const = 0;

    /**
     * @brief Renders one record.
     *
     * The record has to be a function of its index and of the mesh alone: a caller whose buffer cut the previous record
     * in half causes the same record to be rendered again.
     *
     * @param index The record's index, counting from 0.
     * @param out Scratch space of maxRecordSize() bytes.
     * @return The number of bytes the record holds, or 0 when it cannot be rendered (which fails the pull).
     */
    virtual std::size_t renderRecord(std::uint64_t index, std::span<char> out) = 0;

  private:
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_MESHIO_NS_END
