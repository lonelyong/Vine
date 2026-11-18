#include <vine/backend_vk/internal/ShaderCompiler.hpp>

#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

/**
 * @brief Reads back from the compiled words WHICH vertex-input locations the stage declares.
 *
 * @details
 * - WHY THE BACKEND READS ITS OWN OUTPUT. A pipeline may declare fewer vertex attributes than the geometry carries,
 *   and the honest set is the one the VERTEX STAGE reads: declaring the others costs a validation warning ("attribute
 *   at location N not consumed by vertex shader") and describes a pipeline nobody wrote. The compiled module is the
 *   only authority on what the stage reads - the source is conditional, so it cannot be asked directly.
 * - WHAT IT READS: every `OpVariable` in the Input storage class, its `Location` decoration, and its TYPE - the
 *   decoration answers the FIRST location and the type answers how far the value reaches (a matrix owns one location
 *   per column, the instance stream being the case that matters). Where a type cannot be resolved the answer is left
 *   UNKNOWN, which callers treat as "declares everything" (`G10`: narrowing on a guess would drop real data).
 *
 * @param module The compiled stage; its words are read, its input_locations answered (known when they could be).
 */
void describeInputLocations(SpirvModule& module)
{
    module.input_locations.clear();
    module.input_locations_known = false;

    // SPIR-V's first five words are the header (magic, version, generator, bound, reserved).
    constexpr std::uint32_t kHeaderWords = 5U;
    constexpr std::uint32_t kMagicNumber = 0x07230203U;
    constexpr std::uint32_t kOpTypeBool   = 20U;
    constexpr std::uint32_t kOpTypeInt    = 21U;
    constexpr std::uint32_t kOpTypeFloat  = 22U;
    constexpr std::uint32_t kOpTypeVector = 23U;
    constexpr std::uint32_t kOpTypeMatrix = 24U;
    constexpr std::uint32_t kOpTypePointer = 32U;
    constexpr std::uint32_t kOpVariable  = 59U;
    constexpr std::uint32_t kOpDecorate  = 71U;
    constexpr std::uint32_t kDecorationLocation = 30U;
    constexpr std::uint32_t kStorageClassInput  = 1U;

    const std::vector<std::uint32_t>& words = module.words;
    if (words.size() <= kHeaderWords || words[0] != kMagicNumber) {
        return;
    }

    // The declarations of one module, collected first and joined afterwards: a decoration is not required to sit next
    // to the variable it decorates, and a variable names a POINTER to its type rather than the type itself.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> decorated; // (id, first location)
    std::vector<std::uint32_t>                          inputs;    // the Input-class variables' ids
    std::map<std::uint32_t, std::uint32_t>              pointee;   // pointer type id -> pointee type id
    std::map<std::uint32_t, std::uint32_t>              locations; // type id -> locations it occupies
    std::map<std::uint32_t, std::uint32_t>              input_type; // variable id -> its pointer type id

    for (std::size_t offset = kHeaderWords; offset < words.size();) {
        const std::uint32_t instruction = words[offset];
        const std::uint32_t count       = instruction >> 16U;
        const std::uint32_t opcode      = instruction & 0xffffU;
        if (count == 0U || offset + count > words.size()) {
            module.input_locations.clear(); // truncated module: the answer stays unknown rather than half-read
            return;
        }
        switch (opcode) {
        case kOpDecorate:
            if (count >= 4U && words[offset + 2U] == kDecorationLocation) {
                decorated.emplace_back(words[offset + 1U], words[offset + 3U]);
            }
            break;
        case kOpVariable:
            if (count >= 4U && words[offset + 3U] == kStorageClassInput) {
                inputs.push_back(words[offset + 2U]);
                input_type[words[offset + 2U]] = words[offset + 1U];
            }
            break;
        case kOpTypePointer:
            // [result id, storage class, type id]
            pointee[words[offset + 1U]] = words[offset + 3U];
            break;
        case kOpTypeFloat:
        case kOpTypeInt:
        case kOpTypeBool:
        case kOpTypeVector:
            // A scalar and a vector each occupy exactly one location.
            locations[words[offset + 1U]] = 1U;
            break;
        case kOpTypeMatrix:
            // [result id, column type, column count]: one location PER COLUMN, which is what the instance stream is.
            locations[words[offset + 1U]] = words[offset + 3U];
            break;
        default:
            break;
        }
        offset += count;
    }

    for (const std::uint32_t id : inputs) {
        std::uint32_t first_location = 0U;
        bool          has_location   = false;
        for (const auto& [decorated_id, location] : decorated) {
            if (decorated_id == id) {
                first_location = location;
                has_location   = true;
                break;
            }
        }
        if (!has_location) {
            continue; // a built-in input (gl_VertexIndex and friends): not a vertex attribute, nothing to declare
        }

        const auto  pointer = input_type.find(id);
        const auto  target  = pointer != input_type.end() ? pointee.find(pointer->second) : pointee.end();
        const auto  width   = pointee.end() != target ? locations.find(target->second) : locations.end();
        if (width == locations.end()) {
            module.input_locations.clear(); // an input whose type this function does not size: leave every channel declared
            return;
        }
        for (std::uint32_t step = 0U; step < width->second; ++step) {
            module.input_locations.push_back(first_location + step);
        }
    }

    std::sort(module.input_locations.begin(), module.input_locations.end());
    module.input_locations.erase(std::unique(module.input_locations.begin(), module.input_locations.end()), module.input_locations.end());
    module.input_locations_known = true;
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
    describeInputLocations(out);
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
