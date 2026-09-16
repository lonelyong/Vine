/**
 * @brief The report-once rule as a type: one report per episode, silence in between, re-arm on request.
 *
 * The rule itself is trivial; what made it worth a type is that it used to be a bool plus a convention
 * spread over ten sites, where "report" was an `if (!flag)` and "re-arm" a bare assignment somewhere else —
 * two spellings whose pairing nothing checked. Episodes differ in what ends them (a new frame, a new pass
 * scope, a usable size, every light lit again, a different source), so re-arming stays the CALLER's call and
 * nothing in the type knows about frames or targets.
 */

#include <gtest/gtest.h>

#include <vine/vsg/VsgReportOnce.hpp>

using vine::vsg::ReportOnce;

/**
 * @brief The first call of an episode reports; the rest of it does not.
 */
TEST(ReportOnceTest, OnlyTheFirstCallOfAnEpisodeReports)
{
    ReportOnce reported;

    EXPECT_FALSE(reported.reported());
    EXPECT_TRUE(reported.shouldReport());
    EXPECT_TRUE(reported.reported());
    EXPECT_FALSE(reported.shouldReport());
    EXPECT_FALSE(reported.shouldReport());
    EXPECT_TRUE(reported.reported()) << "a refused report must not re-arm the episode";
}

/**
 * @brief rearm() ends the episode: the same condition reports again.
 */
TEST(ReportOnceTest, ReArmingStartsANewEpisode)
{
    ReportOnce reported;

    EXPECT_TRUE(reported.shouldReport());
    reported.rearm();
    EXPECT_FALSE(reported.reported());
    EXPECT_TRUE(reported.shouldReport());
}

/**
 * @brief Re-arming an unreported episode is a no-op, because the boundary may fire more often than the
 *        condition (every frame a target has a usable size, say).
 */
TEST(ReportOnceTest, ReArmingAnUnreportedEpisodeIsANoOp)
{
    ReportOnce reported;

    reported.rearm();
    EXPECT_FALSE(reported.reported());
    EXPECT_TRUE(reported.shouldReport());
}
