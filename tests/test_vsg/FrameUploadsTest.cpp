/**
 * @brief A frame answers what IT uploaded and released (T25 / T29).
 *
 * The layers keep cumulative counters (`StreamUploads::uploads()` / `uploadedBytes()` / `retired()`,
 * `MaterialImages::built()` / `uploadedBytes()` / `released()`); `FrameReport::uploads` is the DELTA taken across one
 * frame, because "this frame uploaded 3 streams / 128 KiB and released one texture" is the question a HUD, a gate or a
 * profile asks and no single layer can answer. Before any frame it is ABSENT rather than zero: an unmeasured frame must
 * not read like an empty one.
 */

#include <gtest/gtest.h>

#include <cstdint>

#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/backend_vsg/internal/VsgBackend.hpp>

#include "TestFrame.hpp"
#include "TestTriangleContent.hpp"

using vn::test::makeCamera;
using vn::test::makeTriangle;
using vn::test::TriangleFixture;

namespace
{

/** @brief The triangle drawn into a 32x32 off-screen target, one frame at a time (null content = a quiet frame). */
struct UploadsFixture {
    vn::intrusive_ptr<vn::backend_vsg::VsgBackend>   backend;
    vn::intrusive_ptr<vn::graphics::RenderTarget> target;
    vn::intrusive_ptr<vn::graphics::RenderPass>   pass;
    vn::intrusive_ptr<vn::graphics::Camera>       camera;
    vn::graphics::ClearPolicy                     clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };
};

} // namespace

TEST(FrameUploadsTest, TheReportAnswersWhatThisFrameUploadedAndReleased)
{
    vn::graphics::DeviceDesc desc;
    desc.presentation = vn::graphics::DevicePresentation::None; // a device that presents nothing needs no display

    UploadsFixture fixture;
    fixture.backend = vn::intrusive_ptr<vn::backend_vsg::VsgBackend>(new vn::backend_vsg::VsgBackend(desc));
    if (!fixture.backend->initialize() || !fixture.backend->capabilities().render_targets) {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    // BEFORE ANY FRAME: no answer at all - "not measured" and "measured zero" are different facts (see FrameReport).
    EXPECT_FALSE(fixture.backend->frameReport().uploads.has_value()) << "no frame has run yet";

    fixture.target = vn::intrusive_ptr<vn::graphics::RenderTarget>(new vn::graphics::RenderTarget());
    fixture.target->attachColor(vn::graphics::RenderTarget::ColorFormat::RGBA8);
    fixture.target->setSize(32, 32);
    fixture.pass   = vn::intrusive_ptr<vn::graphics::RenderPass>(new vn::graphics::RenderPass());
    fixture.camera = makeCamera();

    const auto drive = [&fixture](const TriangleFixture* content) {
        TestFrame frame;
        frame.pass(fixture.pass.get())
            .target(fixture.target.get())
            .clear(fixture.clear)
            .depth(vn::graphics::DepthMode::TestAndWrite);
        if (content != nullptr) {
            frame.content(content->commands, fixture.camera.get());
        }
        fixture.backend->executeFrame(frame.value(), nullptr);
    };

    // A DRAWN FRAME UPLOADS ITS STREAMS ONCE, and the report says so with counts AND bytes.
    const TriangleFixture triangle = makeTriangle();
    drive(&triangle);
    vn::graphics::FrameReport report = fixture.backend->frameReport();
    ASSERT_TRUE(report.uploads.has_value());
    EXPECT_GT(report.uploads->stream_uploads, 0U) << "the triangle's streams were built this frame";
    EXPECT_GT(report.uploads->stream_bytes, 0U) << "and the report counts the bytes they copied";
    EXPECT_EQ(report.uploads->texture_uploads, 0U) << "this content declares no texture";

    // A STEADY FRAME MOVES NOTHING: the counters are per frame, not a running total.
    drive(&triangle);
    report = fixture.backend->frameReport();
    ASSERT_TRUE(report.uploads.has_value());
    EXPECT_EQ(report.uploads->stream_uploads, 0U) << "the same content uploads nothing twice";
    EXPECT_EQ(report.uploads->stream_bytes, 0U);
    EXPECT_EQ(report.uploads->stream_releases, 0U);

    // A BUFFER EDIT IS A NEW STREAM IDENTITY: the stream layer keys an upload by (buffer, revision), so moving the
    // buffer's revision re-uploads exactly that stream once. (Geometry::setRevision is a different announcement - the
    // geometry's SHAPE - and does not move a stream key.)
    triangle.positions->bumpRevision();
    drive(&triangle);
    report = fixture.backend->frameReport();
    ASSERT_TRUE(report.uploads.has_value());
    EXPECT_EQ(report.uploads->stream_uploads, 1U) << "the edited stream is re-uploaded exactly once";
    EXPECT_GT(report.uploads->stream_bytes, 0U);

    // CONTENT LEAVING THE FRAME RELEASES ITS STREAMS, and the frame that sweeps them says how many: the second
    // triangle is drawn alone, so the first one's entries have no live row and leave in THIS frame.
    const TriangleFixture second = makeTriangle();
    drive(&second);
    report = fixture.backend->frameReport();
    ASSERT_TRUE(report.uploads.has_value());
    EXPECT_GT(report.uploads->stream_uploads, 0U) << "the new content uploads its own streams";
    EXPECT_GT(report.uploads->stream_releases, 0U) << "and the content that left is reported as released";

    fixture.backend->shutdown();
}
