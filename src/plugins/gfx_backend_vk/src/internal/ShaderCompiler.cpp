#include <vine/backend_vk/internal/ShaderCompiler.hpp>

#include <mutex>
#include <span>
#include <string>
#include <string_view>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

VN_BACKEND_VK_NS_BEGIN

namespace
{

/// @brief glslang's process-wide initialization, run once before the first stage is parsed.
std::once_flag s_glslangInit;

/**
 * @brief Maps an SDK stage kind to glslang's language enum.
 *
 * @param type The SDK stage kind.
 * @return The glslang language, or `EShLangCount` for a stage this backend does not compile.
 */
[[nodiscard]] EShLanguage languageOf(vn::graphics::ShaderStageType type) noexcept
{
    switch (type) {
    case vn::graphics::ShaderStageType::Vertex: return EShLangVertex;
    case vn::graphics::ShaderStageType::Fragment: return EShLangFragment;
    case vn::graphics::ShaderStageType::Compute: return EShLangCompute;
    }
    return EShLangCount;
}

/**
 * @brief Builds the message a failed compilation reports: glslang's log, or a statement that there was none.
 *
 * @param log glslang's info log (may be null).
 * @return The diagnostic text.
 */
[[nodiscard]] vn::String messageOf(const char* log)
{
    if (log == nullptr || *log == '\0') {
        return vn::String(u8"glslang refused the stage without a message");
    }
    return vn::String(reinterpret_cast<const char8_t*>(log));
}

} // namespace

namespace
{

/**
 * @brief The stage's compilation, under @p defines (see the header's two entries).
 *
 * @param stage   The stage to compile.
 * @param defines The names to define before the source, in order.
 * @param out     Receives the SPIR-V words.
 * @param why     Receives the failure's reason.
 * @return true when the stage compiled and linked.
 */
[[nodiscard]] bool compileWithDefines(const vn::graphics::ShaderStage& stage,
                                      const std::span<const std::string> defines,
                                      SpirvModule&                       out,
                                      vn::String&                        why)
{
    why = vn::String();
    out.words.clear();

    if (stage.source.empty()) {
        why = vn::String(u8"the stage carries no GLSL source");
        return false;
    }

    const EShLanguage language = languageOf(stage.type);
    if (language == EShLangCount) {
        why = vn::String(u8"vk: only vertex, fragment and compute stages are compiled (M2 uses the first two)");
        return false;
    }

    std::call_once(s_glslangInit, []() { glslang::InitializeProcess(); });

    // THE TEXT IS HANDED OVER AS BYTES: String stores UTF-8 and std_str_view() aliases that storage, so nothing is
    // converted. The length is passed too, because a shader source is not required to be terminated.
    const std::string_view text = stage.source.std_str_view();

    glslang::TShader shader(language);
    const char* const strings[1]{ text.data() };
    const int         lengths[1]{ static_cast<int>(text.size()) };
    shader.setStringsWithLengths(strings, lengths, 1);
    shader.setEntryPoint(stage.entryPoint.empty() ? "main" : stage.entryPoint.as_std_str().c_str());

    // THE VARIANT'S NAMES, as a preamble: glslang processes it before the source and without disturbing the
    // `#version` line, which is exactly what a `#define` set has to be. No stage reads a value, so each name is
    // defined as 1 (see the header).
    std::string preamble;
    if (!defines.empty()) {
        for (const std::string& name : defines) {
            preamble += "#define ";
            preamble += name;
            preamble += " 1\n";
        }
        shader.setPreamble(preamble.c_str());
    }

    // THE VULKAN TARGET, exactly what `glslangValidator -V` states: SPIR-V 1.0 for Vulkan 1.0. A stage written for a
    // higher Vulkan version still parses (the text's own #version governs the language), and the device this backend
    // creates asks for no feature that a higher SPIR-V version would be needed for.
    shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
        why = messageOf(shader.getInfoLog());
        return false;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        why = messageOf(program.getInfoLog());
        return false;
    }

    glslang::SpvOptions options;
    options.generateDebugInfo = false;
    // THE OPTIMIZER STAYS OFF: an in-tree glslang may be built with ENABLE_OPT=OFF (see cmake/VineGlslang.cmake), and
    // SPIRV-Tools would then be a symbol nobody links. Unoptimized SPIR-V is valid driver input.
    options.disableOptimizer = true;
    options.validate         = false;
    glslang::GlslangToSpv(*program.getIntermediate(language), out.words, &options);

    if (out.words.empty()) {
        why = vn::String(u8"glslang produced no SPIR-V for the stage");
        return false;
    }
    return true;
}

} // namespace

bool compileStage(const vn::graphics::ShaderStage& stage, SpirvModule& out, vn::String& why)
{
    return compileWithDefines(stage, {}, out, why);
}

bool compileStage(const vn::graphics::ShaderStage& stage, const std::span<const std::string> defines, SpirvModule& out, vn::String& why)
{
    return compileWithDefines(stage, defines, out, why);
}

VN_BACKEND_VK_NS_END
