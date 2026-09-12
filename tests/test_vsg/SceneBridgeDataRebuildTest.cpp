/**
 * @brief What a DATA rebuild must and must not redo.
 *
 * A data rebuild re-materialises the whole data node, but only some of its channels come from the model.
 * The white colour carrier, the zero UV array a mesh without UVs binds and the normals DERIVED when the
 * geometry authors none are BUILT, and each costs a pass over the vertices plus an allocation. Building
 * them again for an edit that changed none of their inputs is the waste these tests pin: the bridge
 * remembers them per retained item, keyed by exactly the inputs they were derived from.
 *
 * Asserted by the ADDRESS of the bound array, because that is what distinguishes "reused what it already
 * built" from "built an identical copy again" — the two draw the same picture, so no pixel test can tell
 * them apart, and the second one is the cost being avoided.
 */

#include <gtest/gtest.h>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/vsg/SceneBridge.hpp>

#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Commands.h>
#include <vsg/io/Options.h>
#include <vsg/nodes/Group.h>
#include <vsg/utils/ShaderSet.h>

#include <cstddef>
#include <vector>

using namespace vine::graphics;
using vine::math::Mat4d;

namespace
{

/// Binding indices of the canonical arrays, in the order the bridge binds them.
constexpr std::size_t kBindingPositions = 0u;
constexpr std::size_t kBindingNormals   = 1u;
constexpr std::size_t kBindingTexcoords = 2u;
constexpr std::size_t kBindingColors    = 3u;

/// Builds @p floats as a buffer that owns them.
vine::intrusive_ptr<const vine::Buffer<float>> packed(std::vector<float> floats)
{
    return vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(std::move(floats)));
}

/// Unit triangle at @p x, positions only: no normals, no UVs, no colour, so every optional channel is BUILT.
GeometryPtr triangle(float x)
{
    auto geometry = GeometryPtr(new Geometry());
    geometry->setPositions(packed({ x, 0.0f, 0.0f, x, 1.0f, 0.0f, x, 0.0f, 1.0f }));
    return geometry;
}

/// Finds the vertex Data bound at binding @p binding under a retained subtree.
vsg::Data* findBoundData(vsg::Node* node, std::size_t binding)
{
    if (node == nullptr) {
        return nullptr;
    }
    if (auto bvb = node->cast<vsg::BindVertexBuffers>()) {
        if (binding < bvb->arrays.size() && bvb->arrays[binding] != nullptr && bvb->arrays[binding]->data != nullptr) {
            return bvb->arrays[binding]->data;
        }
        return nullptr;
    }
    if (auto commands = node->cast<vsg::Commands>()) {
        for (const auto& child : commands->children) {
            if (auto* hit = findBoundData(child.get(), binding)) {
                return hit;
            }
        }
    }
    if (auto group = node->cast<vsg::Group>()) {
        for (const auto& child : group->children) {
            if (auto* hit = findBoundData(child.get(), binding)) {
                return hit;
            }
        }
    }
    return nullptr;
}

/// Drives one sync of @p commands through a bridge and returns the retained root.
void sync(vine::vsg::SceneBridge& bridge, vsg::Group& root, const std::vector<RenderCommand>& commands)
{
    bridge.syncRenderCommands(commands, &root, nullptr);
}

}  // namespace

/**
 * @brief An edit that changes none of the derived channels' inputs does not rebuild them.
 *
 * The scenario is the everyday one: a custom channel is swapped (or any other edit that leaves the
 * positions and indices alone), which bumps the geometry's revision and therefore rebuilds the data node.
 * The three BUILT channels must come back as the very same arrays — the geometry's vertices did not change,
 * so neither did their derived normals, their white carrier or their zero UVs.
 */
TEST(SceneBridgeDataRebuildTest, DerivedChannelsSurviveAnUnrelatedRebuild)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto geometry = triangle(0.0f);
    auto material = MaterialPtr(new Material());

    geometry->addBuffer(5u, AttributeBuffer::packed({ 1.0f, 0.0f, 0.0f }, 3u));
    std::vector<RenderCommand> commands;
    commands.emplace_back(geometry, material, Mat4d());
    sync(bridge, *root, commands);

    auto* normals_before   = findBoundData(root.get(), kBindingNormals);
    auto* texcoord_before  = findBoundData(root.get(), kBindingTexcoords);
    auto* color_before     = findBoundData(root.get(), kBindingColors);
    ASSERT_NE(normals_before, nullptr) << "a mesh without normals binds the derived ones";
    ASSERT_NE(texcoord_before, nullptr) << "a mesh without UVs binds the zero array";
    ASSERT_NE(color_before, nullptr) << "a mesh without colour binds the white carrier";

    // Swap the custom channel (positions and indices untouched) and announce the change.
    geometry->addBuffer(5u, AttributeBuffer::packed({ 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f }, 3u));
    geometry->setRevision(geometry->revision() + 1u);
    sync(bridge, *root, commands);

    EXPECT_EQ(findBoundData(root.get(), kBindingNormals), normals_before)
        << "the derived normals were computed from positions that did not change";
    EXPECT_EQ(findBoundData(root.get(), kBindingTexcoords), texcoord_before)
        << "the zero UV array is a function of the vertex count, which did not change";
    EXPECT_EQ(findBoundData(root.get(), kBindingColors), color_before)
        << "the white carrier is a function of the vertex count, which did not change";
}

/**
 * @brief New positions DO rebuild the derived normals (the cache is keyed on them).
 *
 * The control for the test above: reusing derived data is only correct while its inputs are the same, so
 * replacing the positions buffer must produce a freshly derived array — even when the new buffer has the
 * same vertex count.
 */
TEST(SceneBridgeDataRebuildTest, DerivedNormalsFollowThePositionsTheyWereComputedFrom)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto geometry = triangle(0.0f);
    auto material = MaterialPtr(new Material());

    std::vector<RenderCommand> commands;
    commands.emplace_back(geometry, material, Mat4d());
    sync(bridge, *root, commands);

    auto* normals_before = findBoundData(root.get(), kBindingNormals);
    ASSERT_NE(normals_before, nullptr);

    // Same shape, different bytes: a NEW buffer object, which is what the key compares.
    geometry->setPositions(packed({ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f }));
    geometry->setRevision(geometry->revision() + 1u);
    sync(bridge, *root, commands);

    EXPECT_NE(findBoundData(root.get(), kBindingNormals), normals_before)
        << "normals derived from the previous positions must not be reused for new ones";
}

/**
 * @brief A vertex-count change invalidates the size-dependent channels.
 *
 * The white carrier and the zero UV array are sized by the vertex count, so a mesh that grew must not bind
 * the arrays built for the previous size — that would bind too few vertices (or too many) and read the
 * wrong bytes. The check is the address again: a count change must produce new arrays.
 */
TEST(SceneBridgeDataRebuildTest, FallbackChannelsFollowTheVertexCount)
{
    vine::vsg::SceneBridge bridge;
    bridge.setShaderSet(vsg::createPhongShaderSet());
    auto root     = vsg::Group::create();
    auto geometry = triangle(0.0f);
    auto material = MaterialPtr(new Material());

    std::vector<RenderCommand> commands;
    commands.emplace_back(geometry, material, Mat4d());
    sync(bridge, *root, commands);

    auto* texcoord_before = findBoundData(root.get(), kBindingTexcoords);
    auto* color_before    = findBoundData(root.get(), kBindingColors);
    ASSERT_NE(texcoord_before, nullptr);
    ASSERT_NE(color_before, nullptr);

    // Grow the mesh: four vertices instead of three.
    geometry->setPositions(packed({ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f }));
    geometry->setRevision(geometry->revision() + 1u);
    sync(bridge, *root, commands);

    EXPECT_NE(findBoundData(root.get(), kBindingTexcoords), texcoord_before)
        << "the zero array was sized for the previous vertex count";
    EXPECT_NE(findBoundData(root.get(), kBindingColors), color_before)
        << "the white carrier was sized for the previous vertex count";
}
