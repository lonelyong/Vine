#include <vine/vsg/internal/Device.hpp>

#include <vsg/core/Exception.h>

#include <vsg/vk/PhysicalDevice.h>

#include <vine/vsg/internal/DeviceFeatures.hpp>
#include <vine/vsg/internal/DeviceProbe.hpp>

VN_VSG_NS_BEGIN

namespace
{

/** @brief The validation layer, when a caller asks for it. */
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

} // namespace

vn::String deviceFailureMessage(const ::vsg::Exception& error)
{
    // vsg's words first: it is the party that knows what the driver refused (the cap case is vsg's own
    // sentence naming the number). Then the standing fact about THIS build, which is what turns "a device
    // could not be created" into something a host can act on - and the action is the same for every cause
    // that is actually a slot shortage: a render area holds a device for as long as the view is open.
    vn::String message(u8"the render device could not be created: ");
    message += vn::String::fromUtf8(error.message);
    message += u8" — this build allows at most ";
    message += vn::String::fromUtf8(std::to_string(::vsg::Device::maxNumDevices()));
    message += u8" simultaneous render devices (VSG_MAX_DEVICES): a host that keeps one device per render area"
               u8" must close a view before opening another";
    return message;
}

DeviceResult createDevice(const DeviceOptions& options)
{
    DeviceResult result;

    ::vsg::Names layers;
    if (options.validation) {
        layers.push_back(kValidationLayer);
    }

    // The instance version is what the LOADER supports, not what the policy requires: the policy is checked
    // per physical device below, and asking the loader for a version it does not have fails an instance that
    // could have served a conforming device. So the versions are tried downwards and the first that the
    // loader accepts is the one used - the same order the probe walks.
    //
    // `vsg::Instance::create` THROWS on failure rather than returning null, and an exception thrown out of
    // this loop would end the walk at the first version instead of trying the next one: the descending order
    // would exist in the source and nowhere else. The result is caught and turned into "try the next one".
    const auto createInstance = [&layers](std::uint32_t version) -> ::vsg::ref_ptr<::vsg::Instance> {
        try {
            // No window, so no surface and no instance extensions: this is the whole reason the seam exists.
            return ::vsg::Instance::create(::vsg::Names{}, layers, version);
        }
        catch (const ::vsg::Exception&) {
            return nullptr;
        }
    };

    for (const std::uint32_t version : { core::makeApiVersion(1, 4, 0),
                                         core::makeApiVersion(1, 3, 0),
                                         core::makeApiVersion(1, 2, 0),
                                         core::makeApiVersion(1, 1, 0),
                                         core::makeApiVersion(1, 0, 0) })
    {
        result.instance = createInstance(version);
        if (result.instance != nullptr) {
            break;
        }
    }

    // A layer is an INSTRUMENT, not a floor: `VK_LAYER_KHRONOS_validation` is a separate package and many
    // machines (this project's own software-rendering container among them) do not have it. Losing the
    // instrument must not lose the device - but it must not pretend either: a phase that asserts
    // "validation-clean" has to be able to see that the layer is not there, or its three-way evidence
    // (validation and counters and pixels) silently becomes two-way. So the layer is dropped and
    // `DeviceResult::validation` says so.
    if (result.instance == nullptr && !layers.empty()) {
        layers.clear();
        for (const std::uint32_t version : { core::makeApiVersion(1, 4, 0),
                                             core::makeApiVersion(1, 3, 0),
                                             core::makeApiVersion(1, 2, 0),
                                             core::makeApiVersion(1, 1, 0),
                                             core::makeApiVersion(1, 0, 0) })
        {
            result.instance = createInstance(version);
            if (result.instance != nullptr) {
                break;
            }
        }
    }
    if (result.instance == nullptr) {
        result.error = vn::String(u8"no Vulkan loader (no instance could be created)");
        return result;
    }
    result.validation = !layers.empty();

    // The device is chosen by the FLOOR, per device, with the same description the probe uses - and the
    // first device that passes is taken, so a machine with an integrated and a discrete GPU gets the one the
    // loader lists first rather than an arbitrary one.
    ::vsg::ref_ptr<::vsg::PhysicalDevice> chosen;
    int                                   queue_family = -1;
    for (const ::vsg::ref_ptr<::vsg::PhysicalDevice>& physical : result.instance->getPhysicalDevices()) {
        if (physical == nullptr || !api::describePhysicalDevice(*physical).usable) {
            continue;
        }
        const int family = physical->getQueueFamily(VK_QUEUE_GRAPHICS_BIT);
        if (family < 0) {
            continue;
        }
        chosen       = physical;
        queue_family = family;
        break;
    }

    if (chosen == nullptr) {
        result.error = vn::String(u8"no device satisfies the device-floor requirements");
        return result;
    }

    ::vsg::ref_ptr<::vsg::DeviceFeatures> feature_chain = ::vsg::DeviceFeatures::create();
    applyRequiredFeatures(feature_chain);

    const ::vsg::QueueSettings queue_settings{
        ::vsg::QueueSetting{ queue_family, std::vector<float>{ 1.0F } }
    };
    // The extensions the pipeline layer declares dynamic state from (see requiredDeviceExtensions): passing
    // an empty list here is what makes a "dynamic state" the driver may never be told about.
    //
    // vsg REFUSES a creation by THROWING (a `vsg::Exception`, which is not a `std::exception`), and the
    // refusal a caller actually meets is the device cap (see deviceFailureMessage): this seam answers with a
    // reason like every other failure it can have, because "a device could not be created" is a value here,
    // never an exception out of a caller's back.
    try {
        result.device = ::vsg::Device::create(chosen.get(), queue_settings, layers, requiredDeviceExtensions(), feature_chain);
    }
    catch (const ::vsg::Exception& error) {
        result.error = deviceFailureMessage(error);
        return result;
    }
    if (result.device == nullptr) {
        result.error = vn::String(u8"the device could not be created with the required features");
        return result;
    }

    result.queue_family = queue_family;
    result.ok           = true;
    return result;
}

VN_VSG_NS_END
