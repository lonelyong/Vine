#pragma once

#include "core_global.hpp"

#include <array>
#include <cstdint>

#include "String.hpp"

VN_CORE_NS_BEGIN

/**
 * @brief Universally unique identifier (128-bit).
 *
 * Values are created with create() (random version 4) or parsed from canonical
 * text. The default-constructed value is the null (all-zero) UUID.
 */
class VN_CORE_API Uuid {
  public:
    /// Constructs the null (all-zero) UUID.
    Uuid() = default;

    /**
     * @brief Constructs a UUID from its raw 16 bytes.
     *
     * @param bytes The 16 UUID bytes.
     */
    explicit Uuid(const std::array<std::uint8_t, 16>& bytes);

  public:
    /**
     * @brief Creates a new random (version 4) UUID.
     *
     * @return A random UUID.
     */
    static Uuid create();

    /**
     * @brief Returns the null (all-zero) UUID.
     *
     * @return The null UUID.
     */
    static Uuid null();

    /**
     * @brief Parses a canonical "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" UUID.
     *
     * @param text UUID text (UTF-8).
     * @return The parsed UUID; the null UUID on failure.
     */
    static Uuid parse(const String& text);

  public:
    /**
     * @brief Returns the 16 raw UUID bytes.
     *
     * @return The bytes.
     */
    const std::array<std::uint8_t, 16>& bytes() const;

    /**
     * @brief Returns whether this is the null UUID.
     *
     * @return true if all bytes are zero.
     */
    bool isNull() const;

    /**
     * @brief Returns the canonical lowercase "xxxxxxxx-..." text.
     *
     * @return The UUID text.
     */
    String toString() const;

  public:
    bool operator==(const Uuid& other) const;
    bool operator!=(const Uuid& other) const;
    bool operator<(const Uuid& other) const;

  private:
    std::array<std::uint8_t, 16> bytes_{};
};

VN_CORE_NS_END
