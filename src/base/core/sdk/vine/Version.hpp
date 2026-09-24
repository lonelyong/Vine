#pragma once

#include "core_global.hpp"

#include "String.hpp"

VN_CORE_NS_BEGIN

/**
 * @brief Semantic version (major.minor.patch).
 *
 * A version is constructed from its components or parsed from a
 * "major.minor.patch" string. A default-constructed or failed-parse version is
 * invalid (isValid() == false).
 */
class VN_CORE_API Version {
  public:
    /**
     * @brief Constructs an invalid version (isValid() == false).
     */
    Version() = default;

    /**
     * @brief Constructs a valid version from its components.
     *
     * @param major Major version.
     * @param minor Minor version.
     * @param patch Patch version.
     */
    Version(int major, int minor, int patch);

  public:
    /**
     * @brief Parses a "major.minor.patch" version string.
     *
     * @param text Version text (UTF-8).
     * @return The parsed version; invalid on malformed input.
     */
    static Version parse(const String& text);

  public:
    /**
     * @brief Returns the major version.
     *
     * @return The major version.
     */
    int major() const;

    /**
     * @brief Returns the minor version.
     *
     * @return The minor version.
     */
    int minor() const;

    /**
     * @brief Returns the patch version.
     *
     * @return The patch version.
     */
    int patch() const;

    /**
     * @brief Returns whether the version is valid.
     *
     * @return true if valid.
     */
    bool isValid() const;

    /**
     * @brief Returns the canonical "major.minor.patch" text.
     *
     * Empty when the version is invalid.
     *
     * @return The version text.
     */
    String toString() const;

  public:
    /**
     * @brief Compares this version with another.
     *
     * @param other Other version.
     * @return < 0 if lower, 0 if equal, > 0 if higher.
     */
    int compare(const Version& other) const;

    bool operator==(const Version& other) const;
    bool operator!=(const Version& other) const;
    bool operator<(const Version& other) const;
    bool operator<=(const Version& other) const;
    bool operator>(const Version& other) const;
    bool operator>=(const Version& other) const;

  private:
    int  major_ = 0;
    int  minor_ = 0;
    int  patch_ = 0;
    bool valid_ = false;
};

VN_CORE_NS_END
