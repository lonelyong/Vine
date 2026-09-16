#pragma once

/**
 * @brief "Report this condition ONCE per episode" as a value, so the rule has one home.
 *
 * Every diagnostic this backend emits for a condition that PERSISTS is an episode: a frame submitted with
 * none open, a drawing call outside a pass scope, a target whose size is not known yet, a shared depth
 * whose source has not been built yet, a slot with no shader set, a block that had to drop a light, a
 * session device reported once. The rule is the same every time -- report when the condition is first seen,
 * stay silent while it lasts, report again if it ends and comes back.
 *
 * WHAT DIFFERS IS WHEN THE EPISODE ENDS, which is why re-arming is the CALLER's call (each site documents
 * its own boundary: a new frame, a new pass scope, a new target size, every announced light lit again, a
 * different source). The RULE is what is shared, and that is why it is a type instead of a bool plus a
 * convention to remember: `if (!flag) { flag = true; report(...); }` in one place and a bare
 * `flag = false;` in another have to be re-read together at every one of those pairs, and nothing checks
 * that a bare assignment is re-arming rather than forgetting.
 */

#include <vine/vsg/vsg_global.hpp>

V_VSG_NS_BEGIN

class ReportOnce
{
  public:
    /** @brief Gets whether this episode still needs reporting, and records that it was.
     *
     * @return true on the first call of an episode, false for the rest of it.
     */
    [[nodiscard]] bool shouldReport() noexcept
    {
        if (reported_) {
            return false;
        }
        reported_ = true;
        return true;
    }

    /** @brief Gets whether the episode has been reported, without changing it.
     *
     * For the sites and tests that assert on the state (a rule about episodes is not observable otherwise).
     *
     * @return true while the episode has been reported and not re-armed.
     */
    [[nodiscard]] bool reported() const noexcept
    {
        return reported_;
    }

    /** @brief Ends the episode: the same condition reports again.
     *
     * Called by whoever can tell the condition ended -- see the class note: that boundary is each site's.
     */
    void rearm() noexcept
    {
        reported_ = false;
    }

  private:
    bool reported_ = false;
};

V_VSG_NS_END
