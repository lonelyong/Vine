/**
 * @brief "Some announced lights were not lit" episode tests.
 *
 * A pass announces its scene's lights every frame and the slot packs them into its light block
 * (see fillVineLightsBlock) — no substitution, no fallback: the block carries one ambient plus up
 * to three directional lights, so a light that is disabled, is of another kind, is a second
 * ambient, or is the fourth directional simply is not baked. The slot keeps drawing, which is why
 * the case has to be SAID rather than absorbed silently: the picture is not the one the host asked
 * for, and only the counts tell it which.
 *
 * What is pinned here is the REPORTING rule — when a drop is worth a message — not the packing
 * itself (OverlayLightingTest pins the block and its light count). The rule is deliberately not
 * "log every frame with a mismatch": the announced list is rebuilt per frame, so a scene that keeps
 * an unusable light would say the same thing forever. The unit of reporting is an EPISODE.
 */

#include <gtest/gtest.h>

#include <cstddef>

#include <vine/vsg/VsgContentSlot.hpp>

using vine::vsg::detail::beginLightsDroppedEpisode;

/**
 * @brief A PARTIAL drop is an episode too, not only a list whose every entry is unusable.
 *
 * The normal way to hit this is a scene that carries one light the block cannot carry (another
 * kind, or a disabled one) next to usable lights: those lights ARE lit, so nothing used to be
 * reported at all.
 */
TEST(LightDropReportTest, ADroppedLightStartsAnEpisodeEvenWhenOthersStayLit)
{
    bool reported = false;

    EXPECT_TRUE(beginLightsDroppedEpisode(3u, 2u, reported));
    // The same list on the next frame is the SAME episode — the announced list is rebuilt every
    // frame, so an episode may not report more than once — and a deeper drop in it is still that
    // episode (the message carries the counts).
    EXPECT_FALSE(beginLightsDroppedEpisode(3u, 2u, reported));
    EXPECT_FALSE(beginLightsDroppedEpisode(3u, 1u, reported));
    EXPECT_FALSE(beginLightsDroppedEpisode(3u, 0u, reported));
}

/**
 * @brief Every announced light lit again re-arms the report.
 */
TEST(LightDropReportTest, EveryLightLitAgainRearmsTheReport)
{
    bool reported = false;

    EXPECT_TRUE(beginLightsDroppedEpisode(2u, 1u, reported));
    EXPECT_FALSE(beginLightsDroppedEpisode(2u, 2u, reported)); // all lit: re-arm
    EXPECT_TRUE(beginLightsDroppedEpisode(2u, 1u, reported));  // a NEW episode, reported again
    EXPECT_FALSE(beginLightsDroppedEpisode(2u, 1u, reported));
}

/**
 * @brief An empty announcement is not an episode (it is the normal scene-less state).
 */
TEST(LightDropReportTest, AnEmptyAnnouncementIsNotAnEpisode)
{
    bool reported = true; // a previous episode was reported

    // No lights announced: the block falls back to its ambient fill, which is the normal state of
    // a pass whose content scene carries no lights — nothing is dropped, and the report re-arms.
    EXPECT_FALSE(beginLightsDroppedEpisode(0u, 0u, reported));
    EXPECT_FALSE(reported);

    // ...so the next real drop is a new episode.
    EXPECT_TRUE(beginLightsDroppedEpisode(2u, 0u, reported));
}

/**
 * @brief A list whose every entry is unusable stays reported (the original case).
 */
TEST(LightDropReportTest, AWholeListDroppedIsStillReported)
{
    bool reported = false;

    EXPECT_TRUE(beginLightsDroppedEpisode(1u, 0u, reported));
    EXPECT_FALSE(beginLightsDroppedEpisode(1u, 0u, reported));
}
