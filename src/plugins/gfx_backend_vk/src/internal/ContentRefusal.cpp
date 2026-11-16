#include <vine/backend_vk/internal/ContentRefusal.hpp>

VN_BACKEND_VK_NS_BEGIN

const char8_t* contentRefusalCode(const ContentRefusalCause cause) noexcept
{
    // ONE TABLE, NO `default`: a cause added to the enum cannot reach a release without a code here - that is
    // the compile-time half of "a refusal always names its cause" (D-22). The strings are the `vk.content.*`
    // family in scripts/diagnostic_codes.txt and are never reworded.
    switch (cause) {
    case ContentRefusalCause::NoProgram: return u8"vk.content.no_program";
    case ContentRefusalCause::ProgramRefused: return u8"vk.content.program_refused";
    case ContentRefusalCause::ScreenNoProgram: return u8"vk.content.screen_no_program";
    case ContentRefusalCause::ScreenProgramRefused: return u8"vk.content.screen_program_refused";
    case ContentRefusalCause::ScreenPipelineRefused: return u8"vk.content.screen_pipeline_refused";
    case ContentRefusalCause::ScreenNoSource: return u8"vk.content.screen_no_source";
    case ContentRefusalCause::GeometryRefused: return u8"vk.content.geometry_refused";
    case ContentRefusalCause::NoGeometry: return u8"vk.content.no_geometry";
    case ContentRefusalCause::PrimitiveAbsent: return u8"vk.content.primitive_absent";
    case ContentRefusalCause::InstanceStreamRefused: return u8"vk.content.instance_stream_refused";
    case ContentRefusalCause::PipelineRefused: return u8"vk.content.pipeline_refused";
    case ContentRefusalCause::MaterialTextureSkipped: return u8"vk.content.material_texture_skipped";
    case ContentRefusalCause::StandInMissing: return u8"vk.content.stand_in_missing";
    case ContentRefusalCause::UniformRingFull: return u8"vk.content.uniform_ring_full";
    case ContentRefusalCause::DescriptorSetMissing: return u8"vk.content.descriptor_set_missing";
    case ContentRefusalCause::NothingToDraw: return u8"vk.content.nothing_to_draw";
    case ContentRefusalCause::NoRunnablePass: return u8"vk.content.no_runnable_pass";
    case ContentRefusalCause::PlanMissing: return u8"vk.content.plan_missing";
    case ContentRefusalCause::WrongPath: return u8"vk.content.wrong_path";
    case ContentRefusalCause::Count: break; // the sentinel names no cause
    }
    return u8"vk.content.wrong_path"; // unreachable for a valid enumerator; never a silent null
}

VN_BACKEND_VK_NS_END
