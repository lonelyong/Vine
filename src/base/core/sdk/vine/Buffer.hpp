#pragma once
#include "core_global.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "RefCounted.hpp"
#include "intrusive_ptr.hpp"

VN_CORE_NS_BEGIN

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
 * EVERY WRITE ANNOUNCES ITSELF, AND THERE IS ONE WRITE PATH. Growth is announced by this object, not by the
 * writer's memory: a modelling API appends (a mesh builder adds vertices one at a time), so there is an
 * append API and the length is not fixed, and a consumer that cached THE BYTES IT READ FROM THIS BUFFER
 * compares the revision it read against revision() before reusing them — the rule `Texture` and
 * `ShaderProgram` follow for "the same object, new contents". That rule is only honest if no mutation can be
 * silent, and a raw `T*`, an element reference or a span handed out by the buffer itself IS a write this
 * object cannot see — nor can it tell where such an edit begins and ends. So those are handed out by ONE
 * scoped writer instead (see edit()): `Edit` owns the write intent, the buffer announces once when it ends,
 * and no other route to the elements exists. What a consumer must not do is assume the bytes it read are
 * still current: any mutation INVALIDATES byte pointers and views taken before it, so take a view after the
 * edit rather than before.
 *
 * WHAT STAYS MANUAL. A change made OUTSIDE this object — a file mapped elsewhere, a device write into the
 * same allocation, a producer that owns its own version number — cannot be seen here at all, so that writer
 * says so with setRevision(). That is the escape hatch, not the ordinary path.
 *
 * THIS IS THE ANNOUNCEMENT THAT DECIDES RE-UPLOADS. The renderer keys every stream it uploads on
 * (this buffer, this revision, the slice), so moving this number re-sends exactly the streams that read this
 * buffer - one channel of one geometry, or every geometry sharing an arena, because the bytes are shared.
 * Geometry::setRevision() is a different announcement: it is about the geometry's SHAPE (which channels it
 * feeds, where each reads) and says nothing about bytes; a host that edits bytes and announces only on the
 * geometry keeps drawing the bytes it uploaded first - there is no fallback, by design.
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
    {}

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
    {}

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
     * @brief Gets read-only access to the elements.
     *
     * @details
     * - The WRITE face is @ref Edit (see edit()): a pointer this object handed out cannot be seen when it is
     *   written through, so no writable pointer exists outside a scope that announces the edit.
     *
     * @return Pointer to the first element, or null when empty.
     */
    [[nodiscard]] const T* data() const noexcept
    {
        return data_.data();
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
     * @brief Gets a typed READ-ONLY view of the elements.
     *
     * This is the TYPED face of the buffer: the model side keeps `Vec3f`-style access without a cast, and
     * the view is a value (`ptr` + `size`), so it costs nothing and needs no lifetime management of its own.
     * The writable view lives on @ref Edit (see edit()).
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
        static_assert(std::is_trivially_copyable_v<T>, "Buffer::bytes() reinterprets the elements; a non-trivial type has no byte view");

        return std::as_bytes(view());
    }

    /**
     * @brief Gets the content revision.
     *
     * Moved by every mutation this object performs (once per @ref Edit scope, see edit()), so a consumer that
     * cached the bytes can tell "the same buffer, still the same contents" from "the same buffer, new
     * contents". A pointer alone cannot: a cache keyed by the buffer's address would otherwise keep serving the
     * contents it read first. (The counterpart of `Geometry::revision()`, `Texture::revision()` and
     * `ShaderProgram::revision()`.)
     *
     * @details
     * - The counter is ATOMIC and the read is an ACQUIRE: a consumer (the upload cache on the frame's thread, say)
     *   that observes a new revision has the bytes written before the edit scope ended. The writer side is a
     *   release (`bumpRevision` / `setRevision` / `Edit`'s destructor), so the pair is a publish, not a poll of
     *   plain memory.
     * - Reading the bytes BEFORE observing a new revision is not made safe by this: the revision is what says
     *   "these are the new bytes".
     *
     * @return Monotonic revision counter (starts at 0).
     */
    [[nodiscard]] std::uint64_t revision() const noexcept
    {
        return revision_.load(std::memory_order_acquire);
    }

    /**
     * @brief A scoped writer over the elements: the only way to change them.
     *
     * @details
     * - WHY A SCOPE AND NOT A POINTER. The buffer cannot see a write made through a raw `T*`, and it cannot tell
     *   where such an edit begins and ends — so a per-write announcement could only ever be a half-truth (one
     *   path announces, the next stays silent). Handing the pointer out from a scope that ENDS is what gives the
     *   buffer the one fact it was missing: the end of the edit.
     * - ONE ANNOUNCEMENT PER EDIT. The destructor moves revision() once, whatever was done inside the scope —
     *   which is also the granularity a consumer wants: "re-read, I am done".
     * - NOT COPYABLE, ONE AT A TIME. Two live writers would each believe they end the edit, so taking a second
     *   one while the first is alive asserts.
     * - THE ELEMENTS OUTLIVE NOTHING. A pointer or view taken inside the scope may be invalidated by the next
     *   mutation (a growth reallocates); take what you need and finish inside the scope.
     *
     * @warning
     * - Not thread-safe: another thread writing at the same time is a data race, with or without this scope.
     */
    class Edit {
      public:
        /**
         * @brief Opens a write scope over @p buffer; the change is announced when the scope ends.
         *
         * @param buffer Buffer to write.
         */
        explicit Edit(Buffer& buffer) noexcept
          : buffer_(buffer)
        {
            assert(buffer_.editing_ == 0U && "one writer at a time: a second live Edit would announce the end twice");
            buffer_.editing_ = 1U;
        }

        /** @brief Ends the edit and announces it (the one announcement a whole edit costs). */
        ~Edit() noexcept
        {
            buffer_.editing_ = 0U;
            buffer_.bumpRevision();
        }

        Edit(const Edit&)            = delete;
        Edit& operator=(const Edit&) = delete;

      public:
        /**
         * @brief Gets writable access to the elements.
         *
         * @warning
         * - The storage may move on the next growth: use it inside the scope, not after it.
         *
         * @return Pointer to the first element, or null when empty.
         */
        [[nodiscard]] T* data() noexcept
        {
            return buffer_.data_.data();
        }

        /**
         * @brief Gets a typed writable view of the elements.
         *
         * @return View over every element.
         */
        [[nodiscard]] std::span<T> view() noexcept
        {
            return std::span<T>(buffer_.data_.data(), buffer_.data_.size());
        }

        /**
         * @brief Gets one writable element.
         *
         * @param index Element index; must be below `size()`.
         * @return The element.
         */
        [[nodiscard]] T& operator[](size_type index) noexcept
        {
            return buffer_.data_[index];
        }

        /**
         * @brief Gets the element count.
         *
         * @return Number of elements.
         */
        [[nodiscard]] size_type size() const noexcept
        {
            return buffer_.data_.size();
        }

        /**
         * @brief Reserves room for @p count elements.
         *
         * Does not count as a mutation by itself, but it may move the storage, so pointers and views taken
         * earlier are invalidated all the same — take a view after the building is done.
         *
         * @param count Elements to make room for.
         */
        void reserve(size_type count)
        {
            buffer_.data_.reserve(count);
        }

        /**
         * @brief Appends one element.
         *
         * @param value Element to append (copied).
         */
        void push_back(const T& value)
        {
            buffer_.data_.push_back(value);
        }

        /**
         * @brief Appends every element of @p values.
         *
         * @param values Elements to append.
         */
        void append(std::span<const T> values)
        {
            if (values.empty()) {
                return;
            }

            buffer_.data_.insert(buffer_.data_.end(), values.begin(), values.end());
        }

        /** @brief Removes every element. */
        void clear() noexcept
        {
            buffer_.data_.clear();
        }

      private:
        Buffer& buffer_;
    };

    /**
     * @brief Opens the write scope over the elements: the only way to change them, announcing the edit when it ends.
     *
     * @details
     * - The boundary of an edit is the scope: several writes inside one @ref Edit cost ONE revision move, and a
     *   write with no scope cannot happen at all.
     *
     * @return The writer; the change is announced when it goes out of scope.
     */
    [[nodiscard]] Edit edit() noexcept
    {
        return Edit(*this);
    }

    /**
     * @brief Announces a change made to the bytes OUTSIDE this object by reporting the revision.
     *
     * @details
     * - The escape hatch, not the ordinary path: a file mapped elsewhere, a device write into the same
     *   allocation, or a producer that already owns a version number. Everything a caller does through @ref Edit
     *   is announced by that scope, so a call here is only ever about bytes this object did not hand out.
     * - The usual value is `revision() + 1` — for that case prefer bumpRevision(), which cannot move the counter
     *   backwards by accident. The counter is only ever COMPARED, so nothing breaks if it jumps; but lowering it
     *   is a real hazard: a consumer holding a cached revision could then treat old bytes as current.
     *
     * @param revision Revision to report.
     */
    void setRevision(std::uint64_t revision) noexcept
    {
        revision_.store(revision, std::memory_order_release);
    }

    /**
     * @brief Announces a change made outside this object by moving the revision one step forward.
     *
     * @details
     * - The safe spelling of `setRevision(revision() + 1)`: the counter cannot be LOWERED by arithmetic, so a
     *   consumer that cached a revision never treats old bytes as current again.
     *
     * @note
     * - Writing through @ref edit() already announces; calling this as well would move the revision twice, which
     *   costs one redundant re-read and nothing else.
     */
    void bumpRevision() noexcept
    {
        revision_.fetch_add(1U, std::memory_order_release);
    }

  private:
    std::vector<T> data_;
    /// Atomic: the frame's thread reads it while the host owns the bytes (see revision()). The writer side is
    /// release, so a consumer that observes the new number has the bytes written before the announcement.
    std::atomic<std::uint64_t> revision_{ 0 };
    /// Whether a writer scope is open (see Edit): one at a time, so the end of an edit is announced once.
    std::uint32_t editing_   = 0;
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
struct BufferSlice {
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
    [[nodiscard]] static std::size_t resolvedLength(std::size_t buffer_size, std::size_t first, std::size_t count) noexcept
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
    [[nodiscard]] static BufferSlice slice(intrusive_ptr<const Buffer<Element>> buffer, std::size_t first, std::size_t count)
    {
        BufferSlice out;
        out.values = std::move(buffer);
        out.first  = first;
        out.count  = count;
        return out;
    }

    /** @brief Returns the buffer's element count (0 when the segment holds nothing). */
    [[nodiscard]] std::size_t bufferSize() const noexcept
    {
        return values != nullptr ? values->size() : 0u;
    }

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
    [[nodiscard]] bool empty() const noexcept
    {
        return size() == 0u;
    }

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

VN_CORE_NS_END
