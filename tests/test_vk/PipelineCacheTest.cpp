/**
 * @brief The vk backend's PERSISTED pipeline cache: a device-keyed file, and the session that fills it (T44 / F4).
 *
 * WHY THIS FILE EXISTS. A `VkPipelineCache` lives from create to destroy; without a file every new session recompiles
 * every pipeline's SPIR-V. The fix has two halves and both are contracts:
 *  - the FILE (PipelineCacheFile): a key that names the device and driver it came from, a strict reading, and an
 *    atomic write - because handing another driver's cache to `vkCreatePipelineCache` is undefined behaviour, not a
 *    slower start;
 *  - the SESSION (Device + DeviceDesc::cache_directory): a host names WHERE, the backend decides WHAT, and
 *    `pipelineCacheReport` says whether it happened.
 *
 * The codec cases run without a Vulkan device (they are pure functions); the session cases skip on a machine with no
 * device, like every device case in this repository.
 */

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Colorf.hpp>
#include <vine/String.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/math/Matrix4x4.hpp>

#include <vine/backend_vk/internal/Device.hpp>
#include <vine/backend_vk/internal/PipelineCacheFile.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::String;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::FrameDescription;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderCommand;
using vn::graphics::RenderTarget;
using vn::backend_vk::PipelineCacheFile;
using vn::backend_vk::PipelineCacheKey;
using vn::backend_vk::PipelineCacheRefusal;
using vn::intrusive_ptr;

namespace
{

constexpr int kTargetSize = 64;

/// @brief A key that names a device this machine is not (the case that must never be trusted).
[[nodiscard]] PipelineCacheKey foreignKey()
{
    PipelineCacheKey key;
    key.vendor_id      = 0x1234U;
    key.device_id      = 0x5678U;
    key.driver_version = 1U;
    key.api_version    = VK_API_VERSION_1_2;
    key.uuid[0]        = 0xAAU;
    return key;
}

[[nodiscard]] std::vector<std::byte> someBlob()
{
    std::vector<std::byte> blob(64U, std::byte{ 0x5A });
    return blob;
}

/// @brief A directory of its own under the system temp dir, removed when the case ends.
struct TempDirectory {
    std::filesystem::path path{};

    TempDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("vine-vk-pipeline-cache-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    ~TempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    TempDirectory(const TempDirectory&)            = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    /// @brief The directory as the SDK spells it (UTF-8 bytes, see DeviceDesc::cache_directory).
    [[nodiscard]] vn::String asDeviceDesc() const
    {
        const std::u8string text = path.u8string();
        return vn::String::fromUtf8(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
    }

    /** @brief Writes @p bytes to the cache file of this directory (a fake, as another device would have left it). */
    void writeFile(const std::vector<std::byte>& bytes) const
    {
        std::error_code error;
        std::filesystem::create_directories(path, error);
        std::ofstream stream(path / PipelineCacheFile::kFileName, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    [[nodiscard]] bool hasFile() const
    {
        std::error_code error;
        return std::filesystem::exists(path / PipelineCacheFile::kFileName, error);
    }
};

[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget()
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vk_cache_target"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(kTargetSize, kTargetSize);
    return target;
}

constexpr const char8_t* kVertexStage = u8R"(
#version 450
layout(location = 0) in vec3 in_position;
layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelView;
} vn_pc;
void main()
{
    gl_Position = vn_pc.projection * vn_pc.modelView * vec4(in_position, 1.0);
}
)";

constexpr const char8_t* kFragmentStage = u8R"(
#version 450
layout(location = 0) out vec4 out_color;
void main()
{
    out_color = vec4(0.25, 0.5, 0.75, 1.0);
}
)";

[[nodiscard]] intrusive_ptr<vn::graphics::ShaderProgram> makeProgram()
{
    auto program = intrusive_ptr<vn::graphics::ShaderProgram>(new vn::graphics::ShaderProgram());
    program->setName(u8"vk_cache_program");
    program->addStage(vn::graphics::ShaderStage{ vn::graphics::ShaderStageType::Vertex, String(kVertexStage), String(u8"main") });
    program->addStage(vn::graphics::ShaderStage{ vn::graphics::ShaderStageType::Fragment, String(kFragmentStage), String(u8"main") });
    return program;
}

[[nodiscard]] intrusive_ptr<vn::graphics::Geometry> makeTriangle()
{
    std::vector<float> positions{
        -0.75F, -0.75F, 0.5F,
        0.75F,  -0.75F, 0.5F,
        0.0F,   0.75F,  0.5F,
    };
    std::vector<std::uint32_t> indices{ 0U, 1U, 2U };

    auto geometry = intrusive_ptr<vn::graphics::Geometry>(new vn::graphics::Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::move(positions))));
    vn::graphics::Primitive primitive;
    primitive.topology = vn::graphics::Topology::Triangles;
    primitive.indices  = vn::graphics::Primitive::IndexSpan::whole(
        intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::move(indices))));
    geometry->clearPrimitives();
    geometry->addPrimitive(primitive);
    geometry->bumpRevision();
    return geometry;
}

/// @brief One frame that BUILDS A PIPELINE: a cache only becomes non-empty when a pipeline is created.
[[nodiscard]] FrameDescription pipelineFrame(RenderTarget* target)
{
    auto       program  = makeProgram();
    auto       geometry = makeTriangle();
    auto       material = intrusive_ptr<vn::graphics::Material>(new vn::graphics::Material());
    material->setBaseColor(Colorf(0.25F, 0.5F, 0.75F, 1.0F));

    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program              = program;
    command.program_description  = program->description();
    command.material             = material;
    command.material_description = material->description();
    const auto commands = std::make_shared<const std::vector<RenderCommand>>(std::vector<RenderCommand>{ std::move(command) });

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass = vn::test::framePassForTest();

    vn::graphics::PassDraw pass;
    pass.target       = target;
    pass.clear        = vn::graphics::ClearPolicy{};
    pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    vn::graphics::DrawDescription draw;
    draw.kind     = vn::graphics::DrawDescription::Kind::Scene;
    draw.camera   = vn::graphics::CameraSnapshot{};
    draw.commands = commands;
    pass.draws.push_back(std::move(draw));
    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief One device session over @p directory: initialize, one pipeline-building frame, answer the pixels, shut down.
struct Session {
    intrusive_ptr<vn::graphics::RenderBackend> backend;
    intrusive_ptr<RenderTarget>                target;
    std::vector<std::uint8_t>                  pixels;

    [[nodiscard]] bool run(const vn::String& cache_directory)
    {
        backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr, cache_directory });
        if (backend == nullptr || !backend->initialize()) {
            return false;
        }
        target = makeTarget();
        const FrameDescription frame = pipelineFrame(target.get());
        (void)backend->executeFrame(frame, nullptr);
        (void)backend->readColorBuffer(target.get(), 0, pixels);
        backend->shutdown();
        return true;
    }

    [[nodiscard]] vn::backend_vk::Device::PipelineCacheReport report() const
    {
        const auto* device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
        return device != nullptr ? device->pipelineCacheReport() : vn::backend_vk::Device::PipelineCacheReport{};
    }
};

} // namespace

TEST(VkPipelineCacheFileTest, TheSameKeyRoundTripsAndIsHandedBackIntact)
{
    const std::vector<std::byte> blob   = someBlob();
    const std::vector<std::byte> file    = PipelineCacheFile::encode(foreignKey(), blob);
    PipelineCacheRefusal         refusal = PipelineCacheRefusal::Missing;

    const std::vector<std::byte> decoded = PipelineCacheFile::decode(file, foreignKey(), refusal);
    EXPECT_EQ(refusal, PipelineCacheRefusal::Ok);
    EXPECT_EQ(decoded, blob) << "the blob did not survive the file wrapper";
}

TEST(VkPipelineCacheFileTest, AnotherDevicesFileIsRefused)
{
    // THE CONTRACT THAT MATTERS MOST: a cache from another device or driver is UNDEFINED BEHAVIOUR to hand to
    // vkCreatePipelineCache, so it must be refused on the key - not merely "usually works".
    const std::vector<std::byte> file = PipelineCacheFile::encode(foreignKey(), someBlob());

    PipelineCacheKey     this_device = foreignKey();
    this_device.device_id            = 0x9999U; // same vendor and driver, another device
    PipelineCacheRefusal refusal     = PipelineCacheRefusal::Missing;

    EXPECT_TRUE(PipelineCacheFile::decode(file, this_device, refusal).empty());
    EXPECT_EQ(refusal, PipelineCacheRefusal::ForeignDevice);
}

TEST(VkPipelineCacheFileTest, ATruncatedOrTrailingFileIsRefused)
{
    const std::vector<std::byte> file = PipelineCacheFile::encode(foreignKey(), someBlob());

    PipelineCacheRefusal refusal = PipelineCacheRefusal::Ok;
    std::vector<std::byte> truncated(file.begin(), file.end() - 1);
    EXPECT_TRUE(PipelineCacheFile::decode(truncated, foreignKey(), refusal).empty());
    EXPECT_EQ(refusal, PipelineCacheRefusal::NotACache) << "a cut-off write was trusted";

    std::vector<std::byte> extended = file;
    extended.push_back(std::byte{ 0x00 });
    EXPECT_TRUE(PipelineCacheFile::decode(extended, foreignKey(), refusal).empty());
    EXPECT_EQ(refusal, PipelineCacheRefusal::NotACache) << "trailing bytes were accepted as part of the blob";

    std::vector<std::byte> shorter_than_a_header(file.begin(), file.begin() + 7);
    EXPECT_TRUE(PipelineCacheFile::decode(shorter_than_a_header, foreignKey(), refusal).empty());
    EXPECT_EQ(refusal, PipelineCacheRefusal::NotACache);
}

TEST(VkPipelineCacheFileTest, AnotherFormatVersionOrEmptyBlobIsRefused)
{
    // THE FORMAT VERSION IS THE BACKEND'S OWN PROMISE: bumping it must make old files refuse themselves instead of
    // being half-understood. The version sits right after the 8-byte magic, so flipping it there states version 2.
    std::vector<std::byte> file = PipelineCacheFile::encode(foreignKey(), someBlob());
    file[8]                     = std::byte{ 0x02 };

    PipelineCacheRefusal refusal = PipelineCacheRefusal::Ok;
    EXPECT_TRUE(PipelineCacheFile::decode(file, foreignKey(), refusal).empty());
    EXPECT_EQ(refusal, PipelineCacheRefusal::NotACache);

    const std::vector<std::byte> empty_file = PipelineCacheFile::encode(foreignKey(), {});
    EXPECT_TRUE(PipelineCacheFile::decode(empty_file, foreignKey(), refusal).empty());
    EXPECT_EQ(refusal, PipelineCacheRefusal::Empty);

    TempDirectory directory;
    EXPECT_FALSE(PipelineCacheFile::write(directory.path, foreignKey(), {})) << "an empty blob was written";
    EXPECT_FALSE(directory.hasFile());
}

TEST(VkPipelineCacheFileTest, AFileIsReadOnlyFromTheDirectoryThatHoldsIt)
{
    TempDirectory directory;
    EXPECT_TRUE(PipelineCacheFile::write(directory.path, foreignKey(), someBlob()));
    ASSERT_TRUE(directory.hasFile());

    PipelineCacheRefusal refusal = PipelineCacheRefusal::Ok;
    const auto           blob    = PipelineCacheFile::read(directory.path, foreignKey(), refusal);
    EXPECT_EQ(refusal, PipelineCacheRefusal::Ok);
    EXPECT_EQ(blob, someBlob()) << "what write() put down is not what read() answered";

    PipelineCacheKey another = foreignKey();
    another.vendor_id        = 0x4321U;
    EXPECT_TRUE(PipelineCacheFile::read(directory.path, another, refusal).empty());
    EXPECT_EQ(refusal, PipelineCacheRefusal::ForeignDevice);

    // A DIRECTORY THAT IS NOT THERE IS THE ORDINARY FIRST RUN, not a failure to report as one.
    TempDirectory missing;
    EXPECT_TRUE(PipelineCacheFile::read(missing.path, foreignKey(), refusal).empty());
    EXPECT_EQ(refusal, PipelineCacheRefusal::Missing);
}

TEST(VkPipelineCacheSessionTest, ASessionWritesWhatTheNextOneLoads)
{
    // THE WHOLE POINT OF THE FILE: session one compiles a pipeline and leaves the driver's cache behind, session two
    // starts from it. The numbers are the backend's own (`pipelineCacheReport`), and the picture must not change - a
    // warm start that draws something else would be worse than a cold one.
    TempDirectory directory;
    const String  cache_directory = directory.asDeviceDesc();

    Session first;
    if (!first.run(cache_directory)) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    EXPECT_TRUE(first.report().persisted) << "the stated directory was not used";
    EXPECT_EQ(first.report().loaded_bytes, 0U) << "a first session loaded a cache out of nothing";
    EXPECT_FALSE(first.report().preload_handed) << "a first session handed the driver a cache it did not have";
    EXPECT_FALSE(first.report().discarded);
    EXPECT_GT(first.report().saved_bytes, 0U) << "the session built a pipeline but left no cache";
    EXPECT_EQ(first.report().cache_hits, 0U) << "a first session was served pipelines out of nothing";
    ASSERT_TRUE(directory.hasFile());

    Session second;
    ASSERT_TRUE(second.run(cache_directory));
    EXPECT_TRUE(second.report().persisted);
    EXPECT_FALSE(second.report().discarded) << "the cache this backend wrote was refused by its own writer";
    EXPECT_EQ(second.report().loaded_bytes, first.report().saved_bytes)
        << "the second session did not start from the first session's cache";
    EXPECT_TRUE(second.report().preload_handed)
        << "the cache was read and then never handed to vkCreatePipelineCache";
    // THE HIT IS REPORTED, NOT DEMANDED: a device that answers creation feedback may still be a software rasteriser
    // whose cache is a no-op, and "this driver never reuses" is not this backend's defect. What IS demanded is the
    // direction: a session served from a warm cache cannot report FEWER hits than the same scene run cold.
    if (second.report().hits_measured) {
        EXPECT_GE(second.report().cache_hits, first.report().cache_hits)
            << "a warm session hit the cache less often than a cold one";
    }
    EXPECT_FALSE(second.pixels.empty());
    EXPECT_EQ(second.pixels, first.pixels) << "the warm session drew a different picture";
}

TEST(VkPipelineCacheSessionTest, AFileFromAnotherDeviceIsDiscardedAndReplaced)
{
    // A CACHE THAT IS NOT THIS DEVICE'S IS NEVER TRUSTED, and the session still runs: the report says `discarded`, the
    // frame draws, and the shutdown leaves this device's own cache in the file for the next session.
    TempDirectory directory;
    directory.writeFile(PipelineCacheFile::encode(foreignKey(), someBlob()));

    Session session;
    if (!session.run(directory.asDeviceDesc())) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    EXPECT_TRUE(session.report().persisted);
    EXPECT_TRUE(session.report().discarded) << "another device's cache was accepted";
    EXPECT_EQ(session.report().loaded_bytes, 0U);
    EXPECT_EQ(session.report().cache_hits, 0U) << "a refused cache still served a pipeline";
    EXPECT_GT(session.report().saved_bytes, 0U) << "the refused file was not replaced with this device's own";
    EXPECT_FALSE(session.pixels.empty());

    // ... AND THE FILE REALLY IS THIS DEVICE'S NOW: a fourth session (a third object) loads what the second saved.
    Session again;
    ASSERT_TRUE(again.run(directory.asDeviceDesc()));
    EXPECT_FALSE(again.report().discarded);
    EXPECT_EQ(again.report().loaded_bytes, session.report().saved_bytes);
}

TEST(VkPipelineCacheSessionTest, ACorruptBlobIsRefusedByTheDriverAndTheSessionContinues)
{
    // THE PRELOAD REALLY REACHES THE DRIVER, and a blob it refuses costs a slow start, not a session. The file is
    // session one's own (so the KEY matches and this backend hands it over), with one byte of the BLOB flipped: the
    // driver's own header inside those bytes is then invalid, which it must refuse - that refusal is what a dropped
    // preload cannot produce, so this is the case that pins the hand-over itself.
    TempDirectory directory;
    const String  cache_directory = directory.asDeviceDesc();

    Session first;
    if (!first.run(cache_directory)) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    ASSERT_GT(first.report().saved_bytes, 0U);

    {
        std::ifstream input(directory.path / PipelineCacheFile::kFileName, std::ios::binary | std::ios::ate);
        const std::streamoff size = input.tellg();
        ASSERT_GT(size, 60) << "the cache file is too small for a header plus a blob";
        std::vector<char> raw(static_cast<std::size_t>(size));
        input.seekg(0);
        ASSERT_TRUE(static_cast<bool>(input.read(raw.data(), size)));
        std::vector<std::byte> bytes(raw.size());
        std::memcpy(bytes.data(), raw.data(), raw.size());
        // THE DRIVER'S OWN HEADER, NOT OUR WRAPPER'S: our header is 52 bytes (magic, format, key, blob size), and the
        // blob starts with VkPipelineCacheHeaderVersionOne - headerSize (4), headerVersion (4), then vendorID. Breaking
        // the VENDOR is "this cache is not mine" in the driver's own words, which is the one refusal the spec demands.
        constexpr std::size_t kBlobVendorOffset = 52U + 8U;
        ASSERT_GT(bytes.size(), kBlobVendorOffset);
        bytes[kBlobVendorOffset] = bytes[kBlobVendorOffset] ^ std::byte{ 0xFF };
        directory.writeFile(bytes);
    }

    Session second;
    ASSERT_TRUE(second.run(cache_directory));
    EXPECT_FALSE(second.report().discarded) << "the file was refused by OUR key; the driver's answer is the point";
    EXPECT_GT(second.report().loaded_bytes, 0U);
    EXPECT_TRUE(second.report().preload_handed) << "the corrupt blob never reached the driver";
    // WHETHER THE DRIVER REFUSES IT IS THE DRIVER'S ANSWER: a validating implementation reports `preload_refused`, a
    // permissive one (a software rasteriser copies the bytes and never looks) does not - and EITHER WAY the frame must
    // be the same picture, because a cache is an optimisation and never an input.
    EXPECT_FALSE(second.pixels.empty()) << "a corrupt cache cost the frame";
    EXPECT_EQ(second.pixels, first.pixels) << "a corrupt cache changed what the session drew";
}

TEST(VkPipelineCacheSessionTest, ADeviceWithNoDirectoryKeepsNothing)
{
    // EMPTY DIRECTORY = NO PERSISTENCE, stated rather than implied: nothing is written anywhere, and the report says
    // so with `persisted == false` - the "absent is not zero" rule the rest of this backend's reports follow.
    Session session;
    if (!session.run(String{})) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    EXPECT_FALSE(session.report().persisted);
    EXPECT_FALSE(session.report().discarded);
    EXPECT_EQ(session.report().loaded_bytes, 0U);
    EXPECT_EQ(session.report().saved_bytes, 0U);
}
