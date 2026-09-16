#pragma once
#include "core_global.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "intrusive_ptr.hpp"
#include "RefCounted.hpp"

V_CORE_NS_BEGIN

/**
 * @brief A reference-counted run of T that two consumers can share.
 *
 * WHY IT EXISTS. Vector data used to be held twice: a model keeps its vertices as `std::vector<T>` (typed
 * access is what modelling, collision, picking and IO need) while the renderer needs the same bytes at a
 * layout it can hand to a device. Copying between the two views duplicated every vertex in memory, and for
 * `Vec3f`-like element types the copy produced a BYTE-IDENTICAL block — pure overhead, not a conversion.
 * A shared buffer lets both sides describe ONE allocation: the model keeps typed access through `view()`,
 * and the consumer takes `bytes()`.
 *
 * WHY NOT DERIVE FROM std::vector. `std::vector` has no virtual destructor, so deleting a derived object
 * through a vector reference is undefined behaviour, and passing it anywhere that takes `std::vector&`
 * SLICES the reference count away. Holding a vector as a member gets the same storage without either trap.
 *
 * WHY NOT A `std::shared_ptr<std::vector<T>>`. The repo's rule is `intrusive_ptr` for reference-counted
 * Vine objects and `shared_ptr` for plain data. This is the former: it has identity, is not copyable, and
 * is meant to be shared — exactly the shape `RefCounted` describes.
 *
 * GROWTH IS ANNOUNCED BY THE WRITER, NOT INFERRED HERE. A modelling API appends (a mesh builder adds
 * vertices one at a time), so there is an append API and the length is not fixed. What a consumer must not
 * do is assume the bytes it read are still current: any mutation INVALIDATES byte pointers and views taken
 * before it. But no mutation moves revision() — this object cannot see every write (a non-const `data()`
 * pointer, an element reference, a write from another thread are all invisible to it) and it does not know
 * where an edit begins and ends, so a self-bump could only ever be a half-truth: one write path announces
 * itself, the next one silently does not. The WRITER reports the change with setRevision(), once per edit,
 * and a consumer that cached the bytes compares the revision it read against revision() before reusing them
 * — the rule `Geometry`, `Texture` and `ShaderProgram` already follow for "the same object, new contents".
 * Freezing the length instead would have made the model side unexpressible.
 *
 * @tparam T Element type. Must be trivially copyable for `bytes()` to be meaningful.
 */
template <typename T>
class Buffer : public RefCounted<Buffer<T>> {
  public:
    using value_type = T;
    using size_type  = std::size_t;

  public:
    /** @brief Creates an empty buffer. */
    Buffer() = default;

    /**
     * @brief Creates a buffer of @p count value-initialised elements.
     *
     * @param count Number of elements; may be 0.
     */
    explicit Buffer(size_type count)
      : data_(count)
    {
    }

    /**
     * @brief Adopts an existing vector.
     *
     * The vector is moved in, so a caller that builds data in a `std::vector` (a loader reading a file, a
     * mesh builder) hands the storage over rather than copying it.
     *
     * @param values Storage to adopt.
     */
    explicit Buffer(std::vector<T> values)
      : data_(std::move(values))
    {
    }

  public:
    /**
     * @brief Gets the element count.
     *
     * @return Number of elements.
     */
    [[nodiscard]] size_type size() const noexcept
    {
        return data_.size();
    }

    /**
     * @brief States whether the buffer holds no elements.
     *
     * @return true when the count is 0.
     */
    [[nodiscard]] bool empty() const noexcept
    {
        return data_.empty();
    }

    /**
     * @brief Gets writable access to the elements.
     *
     * Writing through this pointer CANNOT be seen by the buffer, so a caller that does it must announce the
     * change with setRevision() — otherwise a consumer that compares revisions keeps reusing what it read.
     * Every other write path follows the same rule (nothing here moves the revision by itself), which is what
     * makes the rule absolute instead of "some writes announce themselves".
     * This is the same reason the write path exists at all: the model side writes vertices in place far more
     * often than it rebuilds the array.
     *
     * @return Pointer to the first element, or null when empty.
     */
    [[nodiscard]] T* data() noexcept
    {
        return data_.data();
    }

    /**
     * @brief Gets read-only access to the elements.
     *
     * @return Pointer to the first element, or null when empty.
     */
    [[nodiscard]] const T* data() const noexcept
    {
        return data_.data();
    }

    /**
     * @brief Gets one element.
     *
     * @param index Element index; must be below `size()`.
     * @return The element.
     */
    [[nodiscard]] T& operator[](size_type index) noexcept
    {
        return data_[index];
    }

    /**
     * @brief Gets one element, read-only.
     *
     * @param index Element index; must be below `size()`.
     * @return The element.
     */
    [[nodiscard]] const T& operator[](size_type index) const noexcept
    {
        return data_[index];
    }

    /**
     * @brief Gets a typed view of the elements.
     *
     * This is the TYPED face of the buffer: the model side keeps `Vec3f`-style access without a cast, and
     * the view is a value (`ptr` + `size`), so it costs nothing and needs no lifetime management of its own.
     *
     * @return View over every element.
     */
    [[nodiscard]] std::span<T> view() noexcept
    {
        return std::span<T>(data_.data(), data_.size());
    }

    /**
     * @brief Gets a typed read-only view of the elements.
     *
     * @return View over every element.
     */
    [[nodiscard]] std::span<const T> view() const noexcept
    {
        return std::span<const T>(data_.data(), data_.size());
    }

    /**
     * @brief Gets the byte view a device-side consumer uploads.
     *
     * This is the TYPE-ERASED face of the buffer, and it is the reason a single buffer can feed both sides:
     * the bytes ARE the typed elements, so nothing is converted, copied or repacked — the consumer only has
     * to be told the layout (which element type, how many components, what stride).
     *
     * @return The elements as raw bytes.
     */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "Buffer::bytes() reinterprets the elements; a non-trivial type has no byte view");

        return std::as_bytes(view());
    }

    /**
     * @brief Gets the content revision.
     *
     * Moved ONLY by setRevision(), so a consumer that cached the bytes can tell "the same buffer, still the
     * same contents" from "the same buffer, new contents". A pointer alone cannot: a cache keyed by the
     * buffer's address would otherwise keep serving the contents it read first. (The counterpart of
     * `Geometry::revision()`, `Texture::revision()` and `ShaderProgram::revision()`.)
     *
     * @return Monotonic revision counter (starts at 0).
     */
    [[nodiscard]] std::uint64_t revision() const noexcept
    {
        return revision_;
    }

    /**
     * @brief Announces a content change by reporting the revision.
     *
     * The ONLY way the revision moves: mutating the elements (push_back / append / clear, or a write through
     * `data()` / `operator[]` / `view()`) does NOT move it. This object cannot see every write and cannot tell
     * where an edit begins and ends, so it must not guess — the WRITER knows, and says so once per EDIT rather
     * than once per element. That is also the granularity a consumer wants: "re-read, I am done".
     *
     * The usual value is `revision() + 1` — for that case prefer bumpRevision(), which cannot move the
     * counter backwards by accident. The counter is only ever COMPARED, so nothing breaks if it jumps;
     * but lowering it is a real hazard: a consumer holding a cached revision could then treat old bytes as
     * current, or vice versa. Treat it as monotonic unless a caller genuinely needs otherwise.
     *
     * @param revision Revision to report.
     */
    void setRevision(std::uint64_t revision) noexcept
    {
        revision_ = revision;
    }

    /**
     * @brief Announces a content change by moving the revision one step forward.
     *
     * The safe spelling of `setRevision(revision() + 1)`: one call reports an edit, and the counter cannot
     * be LOWERED by arithmetic (see setRevision, where lowering is the hazard). Use this for the ordinary
     * "I edited the contents" case; setRevision stays for a writer that has a revision of its own to
     * report (a model's version, a file's).
     */
    void bumpRevision() noexcept
    {
        ++revision_;
    }

    /**
     * @brief Reserves room for @p count elements.
     *
     * Does NOT count as a mutation of the contents, but it may move the storage, so views taken earlier are
     * invalidated all the same — take a view after the building is done.
     *
     * @param count Elements to make room for.
     */
    void reserve(size_type count)
    {
        data_.reserve(count);
    }

    /**
     * @brief Appends one element.
     *
     * Does not announce the change: report it with setRevision() once the edit is done (see the class
     * comment). A holder of the storage cannot tell "one more element" from "still the same" by itself.
     *
     * @param value Element to append (copied).
     */
    void push_back(const T& value)
    {
        data_.push_back(value);
    }

    /**
     * @brief Appends every element of @p values.
     *
     * Does not announce the change (see push_back).
     *
     * @param values Elements to append.
     */
    void append(std::span<const T> values)
    {
        if (values.empty()) {
            return;
        }

        data_.insert(data_.end(), values.begin(), values.end());
    }

    /**
     * @brief Removes every element.
     *
     * Does not announce the change: an emptied buffer is a change a holder has to hear about too.
     */
    void clear() noexcept
    {
        data_.clear();
    }

  private:
    std::vector<T> data_;
    std::uint64_t  revision_ = 0;
};

/**
 * @brief A SEGMENT of a shared buffer: which buffer, where the segment starts, how much it covers.
 *
 * WHY IT EXISTS. A consumer that reads part of a buffer used to carry the parts as loose members —
 * a buffer pointer plus a first index plus a count — and every such consumer re-derived the same
 * three rules (an offset past the end clamps, a count of 0 means "the rest of the buffer", and the
 * length is resolved against the buffer as it is NOW, not as it was when the segment was named).
 * Four copies of that arithmetic is four chances to disagree, and the disagreements are silent: a
 * stream that reads one element too few still draws.
 *
 * So the segment is a value: a buffer, a first element and an element count, with ONE definition
 * of what it covers. A vertex attribute channel, an index stream and any stream added later are
 * the same shape, which is what lets a consumer walk a geometry's streams without a special case
 * for the index one.
 *
 * THE BUFFER IS RE-READ, NEVER SNAPSHOTTED. The buffer may grow (a modelling API appends), and a
 * segment that follows it is what makes "the whole buffer" usable as an arena's growing tail. A
 * consumer that cached the bytes compares `Buffer::revision()` before reusing them, exactly as it
 * does for a whole buffer.
 *
 * @tparam Element Element type of the buffer's run.
 */
template <typename Element>
struct BufferSlice
{
    /// The buffer, or null when the segment holds nothing. Not snapshotted: every accessor reads through it.
    intrusive_ptr<const Buffer<Element>> values;
    /// First element of the segment (0 = the buffer's start). Past the end clamps to the end.
    std::size_t first{ 0 };
    /// Elements the segment covers, or 0 for "the rest of @p values from @p first" (the growing case).
    std::size_t count{ 0 };

    /**
     * @brief The ONE definition of how many elements a segment covers.
     *
     * @param buffer_size Elements in the buffer.
     * @param first       First element of the segment.
     * @param count       Elements the segment states, or 0 for the rest.
     * @return Elements readable from @p first (0 when @p first is at or past the end).
     */
    [[nodiscard]] static std::size_t resolvedLength(std::size_t buffer_size, std::size_t first,
                                                    std::size_t count) noexcept
    {
        const std::size_t begin     = first > buffer_size ? buffer_size : first;
        const std::size_t available = buffer_size - begin;
        return count == 0u ? available : (count > available ? available : count);
    }

    /**
     * @brief A segment covering all of @p buffer.
     *
     * @param buffer Buffer to read, or null for an empty segment.
     * @return The segment.
     */
    [[nodiscard]] static BufferSlice whole(intrusive_ptr<const Buffer<Element>> buffer)
    {
        BufferSlice out;
        out.values = std::move(buffer);
        return out;
    }

    /**
     * @brief A segment of @p buffer starting at @p first, covering @p count elements.
     *
     * The entry point for an ARENA: one buffer holds several consumers' elements, and each states
     * the segment it reads in the unit it authors them in (vertices, indices, elements).
     *
     * @param buffer Buffer to read, or null for an empty segment.
     * @param first  First element of the segment.
     * @param count  Elements the segment covers, or 0 for the rest of @p buffer from @p first.
     * @return The segment.
     */
    [[nodiscard]] static BufferSlice slice(intrusive_ptr<const Buffer<Element>> buffer, std::size_t first,
                                           std::size_t count)
    {
        BufferSlice out;
        out.values = std::move(buffer);
        out.first  = first;
        out.count  = count;
        return out;
    }

    /** @brief Returns the buffer's element count (0 when the segment holds nothing). */
    [[nodiscard]] std::size_t bufferSize() const noexcept { return values != nullptr ? values->size() : 0u; }

    /** @brief Returns the first element this segment reads, clamped to the buffer's current end. */
    [[nodiscard]] std::size_t begin() const noexcept
    {
        const std::size_t size = bufferSize();
        return first > size ? size : first;
    }

    /** @brief Returns how many elements this segment covers (see resolvedLength). */
    [[nodiscard]] std::size_t size() const noexcept
    {
        return resolvedLength(bufferSize(), first, count);
    }

    /** @brief Returns whether the segment covers no elements (a null buffer counts as empty). */
    [[nodiscard]] bool empty() const noexcept { return size() == 0u; }

    /**
     * @brief Returns the segment as a view over the buffer.
     *
     * @return The segment's elements, empty when it holds none. It is a view over the buffer, so it
     *         must not outlive it.
     */
    [[nodiscard]] std::span<const Element> span() const
    {
        if (values == nullptr) {
            return {};
        }
        return values->view().subspan(begin(), size());
    }
};

V_CORE_NS_END
