/**
 * @brief The frame's mapped block storage, on a real device.
 *
 * The properties these cases pin are the ones a steady frame depends on: every block lands inside its own
 * region of ONE buffer, the rings rotate by slab so a frame writes where the frames in flight are not, a
 * steady frame reuses the same bytes, and a material whose revision did not move writes NOTHING (while an
 * edit writes exactly one block). Refusals are pinned too: a block past its region's stride and a frame past
 * its block budget are counted, never accommodated by growing - growth would move bytes a submitted command
 * buffer still names.
 *
 * A window is how the backend reaches a device (`Window::create` then `getOrCreateDevice()`, exactly what
 * the session does), so this test creates one instead of inventing a second way to bring up Vulkan. X11 +
 * a device that satisfies the backend's requirements, or it SKIPS: what it checks is this storage's
 * behaviour, not the machine's configuration.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <vsg/app/Window.h>
#include <vsg/app/WindowTraits.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/DeviceProbe.hpp>

using vine::vsg::BlockStorage;
using vine::vsg::api::probePhysicalDevices;

namespace
{

/// @brief A window (and with it a device) the test owns, plus a storage built on that device.
class Fixture
{
  public:
    /** @brief Whether this machine can run the device cases at all. */
    [[nodiscard]] static bool available()
    {
        return std::getenv("DISPLAY") != nullptr && probePhysicalDevices().usableCount() > 0;
    }

    /** @brief Creates the window, the device and the storage (in that order). */
    bool build(const BlockStorage::Layout& layout)
    {
        auto traits         = ::vsg::WindowTraits::create();
        traits->width       = 64;
        traits->height      = 64;
        traits->windowTitle = "Vine block storage test";
        window              = ::vsg::Window::create(traits);
        if (window == nullptr) {
            return false;
        }
        device = window->getOrCreateDevice();
        if (device == nullptr) {
            return false;
        }
        storage = BlockStorage::create(device, layout);
        return storage != nullptr;
    }

    ~Fixture()
    {
        // The storage dies first: it references the device (its buffer and mapping).
        storage.reset();
        device.reset();
        window.reset();
    }

    ::vsg::ref_ptr<::vsg::Window>  window;
    ::vsg::ref_ptr<::vsg::Device>  device;
    std::unique_ptr<BlockStorage> storage;
};

/// @brief A block whose bytes are all @p value, so a read-back can prove WHICH block landed where.
std::vector<std::byte> blockOf(std::size_t size, std::uint8_t value)
{
    return std::vector<std::byte>(size, static_cast<std::byte>(value));
}

/// @brief Reads @p size bytes back from the mapping at @p offset.
std::vector<std::byte> readBack(const BlockStorage& storage, std::uint64_t offset, std::size_t size)
{
    const std::span<const std::byte> bytes = storage.bytes();
    EXPECT_LE(offset + size, bytes.size()) << "the read-back must stay inside the mapping";
    return std::vector<std::byte>(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                  bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
}

/// @brief Whether @p offset (and its block) falls inside the region [base, base + bytes).
bool inside(std::uint64_t offset, std::size_t size, std::uint64_t base, std::uint64_t bytes)
{
    return offset >= base && offset + size <= base + bytes;
}

}  // namespace

TEST(BlockStorageTest, ANullDeviceYieldsNoStorage)
{
    EXPECT_EQ(BlockStorage::create({}, {}), nullptr) << "no device means no buffer, so the caller gets nothing";
}

TEST(BlockStorageTest, TheRegionsAreLaidOutOnceAndDoNotOverlap)
{
    if (!Fixture::available()) {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }
    Fixture fixture;
    ASSERT_TRUE(fixture.build({})) << "the storage could not be created on this device";

    const BlockStorage::Regions regions = fixture.storage->regions();

    EXPECT_EQ(regions.views_base, 0U) << "the view blocks start the buffer";
    EXPECT_GT(regions.views_bytes, 0U);
    EXPECT_LE(regions.views_bytes, regions.draws_base) << "the view region ends before the draw region";
    EXPECT_GT(regions.draws_bytes, 0U);
    EXPECT_LE(regions.draws_base + regions.draws_bytes, regions.materials_base);
    EXPECT_GT(regions.materials_bytes, 0U);
    EXPECT_EQ(fixture.storage->capacityBytes(), regions.materials_base + regions.materials_bytes);
    EXPECT_EQ(fixture.storage->bytes().size(), fixture.storage->capacityBytes())
        << "the whole buffer is mapped: every write goes straight into it";
    ASSERT_NE(fixture.storage->buffer(), nullptr);
}

TEST(BlockStorageTest, EveryBlockIsReadBackFromInsideItsOwnRegion)
{
    if (!Fixture::available()) {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }
    Fixture fixture;
    ASSERT_TRUE(fixture.build({}));
    const BlockStorage::Regions regions = fixture.storage->regions();

    const std::vector<std::byte> view = blockOf(288, 0x11);
    const std::vector<std::byte> draw = blockOf(80, 0x22);
    const std::vector<std::byte> material = blockOf(64, 0x33);
    const int                 material_identity = 0;

    fixture.storage->beginFrame();
    const auto view_result     = fixture.storage->writeView(view);
    const auto draw_result     = fixture.storage->writeDraw(draw);
    const auto material_result = fixture.storage->writeMaterial(&material_identity, 1, material);

    ASSERT_TRUE(view_result.valid);
    ASSERT_TRUE(draw_result.valid);
    ASSERT_EQ(material_result.kind, vine::vsg::core::MaterialArena::WriteKind::Allocated);
    EXPECT_TRUE(inside(view_result.offset, view.size(), regions.views_base, regions.views_bytes));
    EXPECT_TRUE(inside(draw_result.offset, draw.size(), regions.draws_base, regions.draws_bytes));
    EXPECT_TRUE(inside(material_result.offset, material.size(), regions.materials_base, regions.materials_bytes));

    EXPECT_EQ(readBack(*fixture.storage, view_result.offset, view.size()), view);
    EXPECT_EQ(readBack(*fixture.storage, draw_result.offset, draw.size()), draw);
    EXPECT_EQ(readBack(*fixture.storage, material_result.offset, material.size()), material);
    EXPECT_EQ(fixture.storage->writes(), 3U);
    EXPECT_EQ(fixture.storage->bytesWritten(), view.size() + draw.size() + material.size());
}

TEST(BlockStorageTest, TheSlabsRotateSoASteadyFrameWritesWhereTheFramesInFlightDoNot)
{
    if (!Fixture::available()) {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }
    Fixture fixture;
    BlockStorage::Layout layout;
    layout.views.slots = 3;  // three frames in flight: three slabs
    ASSERT_TRUE(fixture.build(layout));

    const std::vector<std::byte> block = blockOf(288, 0x44);

    std::vector<std::uint64_t> offsets;
    for (int frame = 0; frame < 4; ++frame) {
        fixture.storage->beginFrame();
        offsets.push_back(fixture.storage->writeView(block).offset);
    }

    EXPECT_NE(offsets[0], offsets[1]) << "frame 1 must not write over the frame the GPU may still read";
    EXPECT_NE(offsets[1], offsets[2]);
    EXPECT_EQ(offsets[3], offsets[0]) << "after three frames the first slab is free again";
    EXPECT_EQ(fixture.storage->overflows(), 0U);
}

TEST(BlockStorageTest, AFramePastItsBudgetIsRefusedAndNothingGrows)
{
    if (!Fixture::available()) {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }
    Fixture fixture;
    BlockStorage::Layout layout;
    layout.draws.blocks_per_frame = 2;
    ASSERT_TRUE(fixture.build(layout));

    const std::vector<std::byte> block      = blockOf(80, 0x55);
    const std::uint64_t          capacity   = fixture.storage->capacityBytes();

    fixture.storage->beginFrame();
    EXPECT_TRUE(fixture.storage->writeDraw(block).valid);
    EXPECT_TRUE(fixture.storage->writeDraw(block).valid);
    EXPECT_FALSE(fixture.storage->writeDraw(block).valid) << "the budget is a promise, not a hint";
    EXPECT_EQ(fixture.storage->overflows(), 1U);
    EXPECT_EQ(fixture.storage->capacityBytes(), capacity) << "the buffer did not move";
}

TEST(BlockStorageTest, AnOversizedBlockIsRefusedBeforeItConsumesAnything)
{
    if (!Fixture::available()) {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }
    Fixture fixture;
    ASSERT_TRUE(fixture.build({}));

    fixture.storage->beginFrame();
    const std::vector<std::byte> oversized = blockOf(4096, 0x66);
    const std::vector<std::byte> proper    = blockOf(288, 0x77);

    EXPECT_FALSE(fixture.storage->writeView(oversized).valid);
    EXPECT_EQ(fixture.storage->oversized(), 1U);
    EXPECT_EQ(fixture.storage->writes(), 0U) << "nothing was copied";
    EXPECT_TRUE(fixture.storage->writeView(proper).valid)
        << "a refused oversized block must not have consumed the frame's budget";
}

TEST(BlockStorageTest, ASteadyMaterialWritesNothingAndAnEditWritesExactlyOneBlock)
{
    if (!Fixture::available()) {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }
    Fixture fixture;
    ASSERT_TRUE(fixture.build({}));

    const int                    material = 0;
    const std::vector<std::byte> first    = blockOf(64, 0xA1);
    const std::vector<std::byte> second   = blockOf(64, 0xB2);
    const std::vector<std::byte> third    = blockOf(64, 0xC3);
    const std::vector<std::byte> fourth   = blockOf(64, 0xD4);

    fixture.storage->beginFrame();
    const auto allocated = fixture.storage->writeMaterial(&material, 1, first);
    ASSERT_EQ(allocated.kind, vine::vsg::core::MaterialArena::WriteKind::Allocated);
    EXPECT_EQ(allocated.bytes, first.size());
    EXPECT_EQ(readBack(*fixture.storage, allocated.offset, first.size()), first);

    // The steady frame: the same revision, so the bytes the GPU reads are already right.
    fixture.storage->beginFrame();
    const auto steady = fixture.storage->writeMaterial(&material, 1, second);
    EXPECT_EQ(steady.kind, vine::vsg::core::MaterialArena::WriteKind::Unchanged);
    EXPECT_EQ(steady.bytes, 0U);
    EXPECT_EQ(steady.offset, allocated.offset)
        << "a hit still names the block: a draw binds THAT offset (offset 0 is another region's data)";
    EXPECT_EQ(readBack(*fixture.storage, allocated.offset, first.size()), first)
        << "a steady frame must not have overwritten anything";

    // An edit: exactly one block, wherever this frame's copy lives.
    fixture.storage->beginFrame();
    const auto rewritten = fixture.storage->writeMaterial(&material, 2, third);
    ASSERT_EQ(rewritten.kind, vine::vsg::core::MaterialArena::WriteKind::Rewritten);
    EXPECT_EQ(rewritten.bytes, third.size());
    EXPECT_EQ(readBack(*fixture.storage, rewritten.offset, third.size()), third);

    // A second edit in the SAME frame: the GPU can only ever see one of the two versions, so one write.
    const auto deduped = fixture.storage->writeMaterial(&material, 3, fourth);
    EXPECT_EQ(deduped.kind, vine::vsg::core::MaterialArena::WriteKind::Unchanged);
    EXPECT_EQ(deduped.bytes, 0U);
    EXPECT_EQ(deduped.offset, rewritten.offset) << "the frame's copy is where both answers point";
    EXPECT_EQ(readBack(*fixture.storage, rewritten.offset, third.size()), third)
        << "the frame's copy still holds the version the draw will use";
    EXPECT_EQ(fixture.storage->materialWrites(), 2U);
    EXPECT_EQ(fixture.storage->bytesWritten(), first.size() * 2U);
}

TEST(BlockStorageTest, TheMaterialCopiesRotateSoAnInFlightFrameIsNeverOverwritten)
{
    if (!Fixture::available()) {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }
    Fixture fixture;
    BlockStorage::Layout layout;
    layout.materials.copies = 3;
    ASSERT_TRUE(fixture.build(layout));

    const int material = 0;

    // Three consecutive edits: each frame writes its own copy, because the previous ones are in flight.
    std::vector<std::uint64_t> offsets;
    for (std::uint8_t revision = 1; revision <= 3; ++revision) {
        fixture.storage->beginFrame();
        const std::vector<std::byte> block  = blockOf(64, revision);
        const auto                   result = fixture.storage->writeMaterial(&material, revision, block);
        ASSERT_EQ(result.kind, revision == 1 ? vine::vsg::core::MaterialArena::WriteKind::Allocated
                                             : vine::vsg::core::MaterialArena::WriteKind::Rewritten);
        EXPECT_EQ(readBack(*fixture.storage, result.offset, block.size()), block);
        offsets.push_back(result.offset);
    }

    EXPECT_NE(offsets[0], offsets[1]);
    EXPECT_NE(offsets[1], offsets[2]);
}
