#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <ostream>
#include <utility>

#include <vine/io/io_global.hpp>

V_IO_NS_BEGIN

/**
 * @brief Failure classes reported by virtual file system and stream operations.
 *
 * A failure travels back as a value instead of an exception, so a caller can
 * tell "nothing is there" apart from "the backend broke" and react to each.
 */
enum class IoError : std::uint8_t
{
    Ok,               ///< No error.
    NotFound,         ///< The path does not exist.
    AlreadyExists,    ///< The target already exists.
    PermissionDenied, ///< The storage refused access.
    NotADirectory,    ///< A directory was needed but the path names a file.
    IsADirectory,     ///< A file was needed but the path names a directory.
    NotEmpty,         ///< A directory had to be empty but still holds entries.
    InvalidPath,      ///< The path is not a valid virtual path.
    ReadOnly,         ///< The backend or mount refuses every write.
    IoFailure,        ///< The underlying storage failed.
    Unsupported,      ///< The backend does not implement the operation.
    InvalidData,      ///< Stored data is malformed, e.g. a broken ZIP archive.
    Closed,           ///< The stream or handle is already closed.
    OutOfRange,       ///< An offset or seek position is out of range.
    CapacityExceeded, ///< The operation would exceed a capacity limit.
};

/**
 * @brief Names a failure class.
 *
 * @param error The failure to name.
 * @return A static, human-readable identifier such as "NotFound".
 */
inline const char* ioErrorName(IoError error) noexcept
{
    switch (error) {
    case IoError::Ok: return "Ok";
    case IoError::NotFound: return "NotFound";
    case IoError::AlreadyExists: return "AlreadyExists";
    case IoError::PermissionDenied: return "PermissionDenied";
    case IoError::NotADirectory: return "NotADirectory";
    case IoError::IsADirectory: return "IsADirectory";
    case IoError::NotEmpty: return "NotEmpty";
    case IoError::InvalidPath: return "InvalidPath";
    case IoError::ReadOnly: return "ReadOnly";
    case IoError::IoFailure: return "IoFailure";
    case IoError::Unsupported: return "Unsupported";
    case IoError::InvalidData: return "InvalidData";
    case IoError::Closed: return "Closed";
    case IoError::OutOfRange: return "OutOfRange";
    case IoError::CapacityExceeded: return "CapacityExceeded";
    }
    return "Unknown";
}

/**
 * @brief Writes a failure's name, so diagnostics and test output stay readable.
 *
 * @param out Target output stream.
 * @param error The failure to write.
 * @return The output stream.
 */
inline std::ostream& operator<<(std::ostream& out, IoError error)
{
    return out << ioErrorName(error);
}

/**
 * @brief Holds either a value or an IoError.
 *
 * std::expected is C++23, so this is the minimal stand-in: a value, a failure
 * and the check between them, with no combinators. Construct it from the value
 * to succeed or from an IoError to fail.
 *
 * @tparam T The value type; must be movable.
 */
template <typename T>
class Result
{
  public:
    /**
     * @brief Wraps a successful value.
     *
     * @param value The value to carry.
     */
    Result(T value)
      : value_(std::move(value))
    {
    }

    /**
     * @brief Carries a failure.
     *
     * @param error The failure to report; IoError::Ok is not a failure.
     */
    Result(IoError error)
      : error_(error)
    {
        assert(error != IoError::Ok);
    }

    /**
     * @brief Reports whether a value is carried.
     *
     * @return true on success, false when an error is carried.
     */
    [[nodiscard]] bool ok() const noexcept
    {
        return error_ == IoError::Ok;
    }

    /**
     * @brief Reports whether a value is carried.
     *
     * @return true on success, false when an error is carried.
     */
    explicit operator bool() const noexcept
    {
        return ok();
    }

    /**
     * @brief The carried error.
     *
     * @return The failure, or IoError::Ok when a value is carried.
     */
    [[nodiscard]] IoError error() const noexcept
    {
        return error_;
    }

    /**
     * @brief Accesses the carried value.
     *
     * @return Reference to the carried value.
     * @warning Undefined behaviour unless ok() is true.
     */
    [[nodiscard]] T& value() noexcept
    {
        return *value_;
    }

    /**
     * @brief Accesses the carried value.
     *
     * @return Const reference to the carried value.
     * @warning Undefined behaviour unless ok() is true.
     */
    [[nodiscard]] const T& value() const noexcept
    {
        return *value_;
    }

    /**
     * @brief Moves the carried value out.
     *
     * @return The value, moved from this result.
     * @warning Undefined behaviour unless ok() is true.
     */
    [[nodiscard]] T take()
    {
        return std::move(*value_);
    }

    /**
     * @brief Accesses the carried value.
     *
     * @return Reference to the carried value.
     * @warning Undefined behaviour unless ok() is true.
     */
    [[nodiscard]] T& operator*() noexcept
    {
        return *value_;
    }

    /**
     * @brief Accesses the carried value.
     *
     * @return Const reference to the carried value.
     * @warning Undefined behaviour unless ok() is true.
     */
    [[nodiscard]] const T& operator*() const noexcept
    {
        return *value_;
    }

    /**
     * @brief Accesses a member of the carried value.
     *
     * @return Pointer to the carried value.
     * @warning Undefined behaviour unless ok() is true.
     */
    [[nodiscard]] T* operator->() noexcept
    {
        return &*value_;
    }

    /**
     * @brief Accesses a member of the carried value.
     *
     * @return Pointer to the carried value.
     * @warning Undefined behaviour unless ok() is true.
     */
    [[nodiscard]] const T* operator->() const noexcept
    {
        return &*value_;
    }

  private:
    std::optional<T> value_;
    IoError          error_{ IoError::Ok };
};

V_IO_NS_END
