#pragma once

#include <vine/appfw/appfw_global.hpp>

#include <memory>
#include <span>
#include <vector>

#include <vine/String.hpp>

VN_APPFW_NS_BEGIN

class ConfigManager;

/**
 * @brief The stored configuration values at one moment, kept so a later moment can be compared with it.
 *
 * @details
 * - `Application` takes one right after it loads the configuration, which is what makes "this run started with that
 *   value" an observable fact instead of a guess; `Application::pendingRestartKeys()` is the question built on it.
 * - Comparison is on stored values, not on their effect: an absent key and a key holding its default count as
 *   different. A value with more than one spelling for the same meaning (`general.language`'s "follow the system",
 *   stored or omitted) can therefore report a change although nothing would behave differently.
 * - It is a value-layer type: a snapshot knows nothing about `ConfigItem`s, so it can say which of the keys it is given
 *   changed, but not which keys deserve the question.
 *
 * @note Cheap to hold: the values are the scalars, strings and arrays the configuration already stores.
 */
class VN_APPFW_API ConfigSnapshot
{
  public:
    /**
     * @brief Creates an empty snapshot: every key counts as changed against it.
     */
    ConfigSnapshot();

    /**
     * @brief Copies the values, so the two snapshots can be compared or kept independently.
     *
     * @param other Snapshot to copy.
     */
    ConfigSnapshot(const ConfigSnapshot& other);

    /**
     * @brief Replaces this snapshot's values with a copy of @p other's.
     *
     * @param other Snapshot to copy.
     * @return This snapshot.
     */
    ConfigSnapshot& operator=(const ConfigSnapshot& other);

    /**
     * @brief Takes over another snapshot's values.
     *
     * @param other Snapshot to move from; left empty.
     */
    ConfigSnapshot(ConfigSnapshot&& other) noexcept;

    /**
     * @brief Takes over another snapshot's values, dropping the ones held now.
     *
     * @param other Snapshot to move from; left empty.
     * @return This snapshot.
     */
    ConfigSnapshot& operator=(ConfigSnapshot&& other) noexcept;

    ~ConfigSnapshot();

    /**
     * @brief Returns the keys among @p keys whose stored value is not the one this snapshot holds.
     *
     * @details
     * - A key present on one side and absent on the other differs.
     * - The result keeps the order of @p keys.
     *
     * @param other Values to compare against, usually `ConfigManager::snapshot()` of the live configuration.
     * @param keys  Keys to compare; keys on neither side are never reported.
     * @return The diverged keys, in the order given.
     */
    std::vector<String> divergedKeys(const ConfigSnapshot& other, std::span<const String> keys) const;

  private:
    friend class ConfigManager;

    struct Impl;
    std::unique_ptr<Impl> d;
};

VN_APPFW_NS_END
