/**
 * @brief View light-group substitution tests (the all-unusable fallback).
 *
 * A per-view light group is seeded with a default light (a headlight for the
 * window presenting slot, an ambient fill elsewhere) and replaced each frame by
 * the content scene's lights. Replacement is destructive — the group's children
 * are swapped — so it must be conditional: an announced list that yields no
 * usable light node (empty, every entry disabled, or every entry of a kind the
 * backend does not translate) means "no active light", and vsg's Phong shades a
 * view with no light to black. The group must be left untouched in that case so
 * the seeded default survives, which is the same fallback the fullscreen-program
 * path already applies (see fillLightPushBlock).
 *
 * These tests pin that contract device-free: the light translation is pure data
 * and needs no Vulkan device (see VsgPipelineFactory.hpp).
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include <vsg/core/ref_ptr.h>
#include <vsg/lighting/AmbientLight.h>
#include <vsg/lighting/DirectionalLight.h>
#include <vsg/nodes/Group.h>

#include <vine/graphics/Light.hpp>

#include <vine/vsg/VsgPipelineFactory.hpp>

using namespace vine::graphics;

namespace
{

/**
 * @brief Builds a group carrying one seeded ambient "default" light.
 *
 * @return A group whose single child is the named seed light.
 */
::vsg::ref_ptr<::vsg::Group> makeSeededGroup()
{
    auto group = ::vsg::Group::create();
    auto seed  = ::vsg::AmbientLight::create();
    seed->name = "seed";
    group->addChild(seed);
    return group;
}

/**
 * @brief Returns whether the group still holds the untouched seed.
 *
 * @param group Group to inspect.
 * @return true when the only child is the seeded ambient named "seed".
 */
bool keepsSeed(const ::vsg::Group& group)
{
    if (group.children.size() != 1u) {
        return false;
    }
    auto ambient = group.children.front()->cast<::vsg::AmbientLight>();
    return ambient != nullptr && ambient->name == "seed";
}

}  // namespace

/**
 * @brief An empty announcement keeps the seeded default light.
 */
TEST(LightGroupTest, EmptyListKeepsSeededDefaultLight)
{
    auto group = makeSeededGroup();
    const std::size_t attached = vine::vsg::detail::setGroupLights(group.get(), {});
    EXPECT_EQ(attached, 0u);
    EXPECT_TRUE(keepsSeed(*group));
}

/**
 * @brief A list whose only light is disabled keeps the seeded default.
 *
 * The host disabled a light; it did not ask for an unlit scene. Dropping the
 * seed here is what used to leave the view black with no diagnostic.
 */
TEST(LightGroupTest, AllDisabledLightsKeepSeededDefaultLight)
{
    auto group    = makeSeededGroup();
    auto disabled = Light::createDirectional();
    disabled->setEnabled(false);

    const std::size_t attached = vine::vsg::detail::setGroupLights(group.get(), { disabled.get() });

    EXPECT_EQ(attached, 0u);
    EXPECT_TRUE(keepsSeed(*group));
}

/**
 * @brief Enabled lights replace the seeded default (the intended substitution).
 */
TEST(LightGroupTest, EnabledLightsReplaceSeededDefaultLight)
{
    auto group   = makeSeededGroup();
    auto ambient = Light::createAmbient();
    ambient->setColor(vine::Colorf(0.25f, 0.5f, 0.75f, 1.0f));
    auto sun = Light::createDirectional();

    const std::size_t attached =
        vine::vsg::detail::setGroupLights(group.get(), { ambient.get(), sun.get() });

    EXPECT_EQ(attached, 2u);
    ASSERT_EQ(group->children.size(), 2u);
    auto got_ambient = group->children[0]->cast<::vsg::AmbientLight>();
    ASSERT_NE(got_ambient, nullptr);
    EXPECT_FLOAT_EQ(got_ambient->color.r, 0.25f);
    EXPECT_NE(group->children[1]->cast<::vsg::DirectionalLight>(), nullptr);
}

/**
 * @brief A mixed list attaches only its usable lights and still replaces the seed.
 */
TEST(LightGroupTest, MixedListAttachesOnlyUsableLights)
{
    auto group    = makeSeededGroup();
    auto disabled = Light::createAmbient();
    disabled->setEnabled(false);
    auto sun = Light::createDirectional();

    const std::size_t attached =
        vine::vsg::detail::setGroupLights(group.get(), { disabled.get(), sun.get() });

    EXPECT_EQ(attached, 1u);
    ASSERT_EQ(group->children.size(), 1u);
    EXPECT_NE(group->children.front()->cast<::vsg::DirectionalLight>(), nullptr);
}

/**
 * @brief A null group is ignored (no crash, nothing attached).
 */
TEST(LightGroupTest, NullGroupIsIgnored)
{
    auto ambient = Light::createAmbient();
    EXPECT_EQ(vine::vsg::detail::setGroupLights(nullptr, { ambient.get() }), 0u);
}
