#include <vine/vsg/api/HostReadback.hpp>

#include <string>

#include <vine/vsg/api/OneShot.hpp>

#include <vsg/commands/Commands.h>
#include <vsg/vk/Device.h>

V_VSG_NS_BEGIN

namespace
{


/// @brief Makes sure a copy of the requested attachment exists to read, and submits one when the frame did not.
///
/// The frame's own copy is preferred (it needs no submission). A copy the frame did not make is submitted
/// here, and only for a target that has been DRAWN INTO: the copy commands transition the image from the
/// layout a render pass leaves it in, which a never-rendered image is not in.
HostReadbackRefusal ensureCopy(OffscreenTarget& target, core::ReadbackKind kind, std::uint32_t attachment,
                               ::vsg::Device* device)
{
    const core::ReadbackResult servable = target.readbackResult(core::ReadbackRequest{ kind, attachment });
    if (servable.ok)
    {
        return HostReadbackRefusal::None;  // a frame copied it: the buffer is current
    }
    if (servable.refusal != core::ReadbackRefusal::NotCaptured)
    {
        switch (servable.refusal)
        {
        case core::ReadbackRefusal::UnknownAttachment: return HostReadbackRefusal::UnknownAttachment;
        case core::ReadbackRefusal::UnreadableFormat: return HostReadbackRefusal::UnreadableFormat;
        case core::ReadbackRefusal::None:
        case core::ReadbackRefusal::NotCaptured: break;
        }
        return HostReadbackRefusal::TransferFailed;
    }
    if (!target.written())
    {
        return HostReadbackRefusal::NotRecorded;  // nothing has been recorded into it: nothing to read
    }
    if (device == nullptr)
    {
        return HostReadbackRefusal::NoDevice;
    }
    // Asking for the copy node also marks the attachment captured, so the probe below is meaningful.
    const ::vsg::ref_ptr<::vsg::Node> copy_node =
        kind == core::ReadbackKind::Color ? target.capture(attachment) : target.captureDepth();
    const ::vsg::ref_ptr<::vsg::Commands> commands = copy_node.cast<::vsg::Commands>();
    if (commands == nullptr)
    {
        return HostReadbackRefusal::TransferFailed;  // no copy commands: nothing could ever be read
    }
    if (!submitCommandsOnce(*device, commands))
    {
        return HostReadbackRefusal::TransferFailed;
    }
    return HostReadbackRefusal::None;
}

}  // namespace

HostReadbackRefusal classifyReadback(OffscreenTarget& target, core::ReadbackKind kind,
                                     std::uint32_t attachment) noexcept
{
    const core::ReadbackResult servable = target.readbackResult(core::ReadbackRequest{ kind, attachment });
    if (servable.ok)
    {
        return HostReadbackRefusal::None;
    }
    switch (servable.refusal)
    {
    case core::ReadbackRefusal::UnknownAttachment: return HostReadbackRefusal::UnknownAttachment;
    case core::ReadbackRefusal::UnreadableFormat: return HostReadbackRefusal::UnreadableFormat;
    case core::ReadbackRefusal::NotCaptured:
        // No frame has copied this attachment back. A target that has been drawn into gets its copy made at
        // read time; one that never was is NotRecorded (the SDK's NotReady), never a copy of UNDEFINED memory.
        return target.written() ? HostReadbackRefusal::None : HostReadbackRefusal::NotRecorded;
    case core::ReadbackRefusal::None: break;
    }
    return HostReadbackRefusal::TransferFailed;
}

HostReadbackRefusal readColorAttachment(OffscreenTarget& target, std::uint32_t attachment, ::vsg::Device* device,
                                        std::vector<std::uint8_t>& outPixels)
{
    const HostReadbackRefusal copy =
        ensureCopy(target, core::ReadbackKind::Color, attachment, device);
    if (copy != HostReadbackRefusal::None)
    {
        return copy;
    }
    const core::PixelProbe probe = target.probe(attachment);
    if (!probe.valid())
    {
        return HostReadbackRefusal::TransferFailed;  // the buffer does not hold a whole picture
    }
    outPixels = probe.pixels();
    return HostReadbackRefusal::None;
}

HostReadbackRefusal readDepthAttachment(OffscreenTarget& target, ::vsg::Device* device,
                                        std::vector<float>& outDepths)
{
    const HostReadbackRefusal copy = ensureCopy(target, core::ReadbackKind::Depth, 0U, device);
    if (copy != HostReadbackRefusal::None)
    {
        return copy;
    }
    const core::DepthProbe probe = target.depthProbe();
    if (!probe.valid())
    {
        return HostReadbackRefusal::TransferFailed;
    }
    outDepths = probe.values();
    return HostReadbackRefusal::None;
}

vine::graphics::ReadbackResult readbackResultOf(HostReadbackRefusal refusal) noexcept
{
    switch (refusal)
    {
    case HostReadbackRefusal::None: return vine::graphics::ReadbackResult::Ok;
    case HostReadbackRefusal::NoTarget: return vine::graphics::ReadbackResult::Invalid;
    case HostReadbackRefusal::UnknownTarget:
    case HostReadbackRefusal::NotBuilt:
    case HostReadbackRefusal::NotRecorded: return vine::graphics::ReadbackResult::NotReady;
    case HostReadbackRefusal::UnknownAttachment: return vine::graphics::ReadbackResult::Invalid;
    case HostReadbackRefusal::UnreadableFormat:
    case HostReadbackRefusal::BorrowedDepth:
    case HostReadbackRefusal::NoDevice: return vine::graphics::ReadbackResult::Unsupported;
    case HostReadbackRefusal::TransferFailed: return vine::graphics::ReadbackResult::Failed;
    }
    return vine::graphics::ReadbackResult::Failed;  // an unmapped case is a failure to look at, never "Ok"
}

vine::String readbackRefusalMessage(HostReadbackRefusal refusal, const char* what)
{
    const std::string entry = what != nullptr ? what : "readback";
    switch (refusal)
    {
    case HostReadbackRefusal::None:
        break;
    case HostReadbackRefusal::NoTarget:
        return vine::String(reinterpret_cast<const char8_t*>((entry + ": no target was given, so there is "
                                                              "nothing to read").c_str()));
    case HostReadbackRefusal::UnknownTarget:
        return vine::String(reinterpret_cast<const char8_t*>((entry + ": the target is not held by this "
                                                              "backend (never announced, or released) - "
                                                              "nothing to read").c_str()));
    case HostReadbackRefusal::NotBuilt:
        return vine::String(reinterpret_cast<const char8_t*>((entry + ": the target has no built attachments "
                                                              "yet - nothing to read").c_str()));
    case HostReadbackRefusal::NotRecorded:
        return vine::String(reinterpret_cast<const char8_t*>((entry + ": no frame has drawn into the target "
                                                              "yet - there is nothing to read back").c_str()));
    case HostReadbackRefusal::UnknownAttachment:
        return vine::String(reinterpret_cast<const char8_t*>((entry + ": the target has no such attachment")
                                                                 .c_str()));
    case HostReadbackRefusal::UnreadableFormat:
        return vine::String(reinterpret_cast<const char8_t*>((entry + ": this backend packs RGBA8 colour and "
                                                              "D16/D32/D32F depth only; that attachment's "
                                                              "format cannot be read back").c_str()));
    case HostReadbackRefusal::BorrowedDepth:
        return vine::String(reinterpret_cast<const char8_t*>((entry + ": the depth attachment is BORROWED from "
                                                              "another target; read it through that source")
                                                                 .c_str()));
    case HostReadbackRefusal::NoDevice:
        return vine::String(reinterpret_cast<const char8_t*>((entry + ": there is no device to copy with, and "
                                                              "no frame's copy could serve the request").c_str()));
    case HostReadbackRefusal::TransferFailed:
        return vine::String(reinterpret_cast<const char8_t*>((entry + ": the copy-back buffer does not hold a "
                                                              "readable picture").c_str()));
    }
    return vine::String();
}

V_VSG_NS_END
