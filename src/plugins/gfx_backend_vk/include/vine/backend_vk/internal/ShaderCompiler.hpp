#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>
#include <vector>

#include <vine/String.hpp>
#include <vine/graphics/ShaderProgram.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief One GLSL stage compiled to SPIR-V words, ready for `vkCreateShaderModule`.
 *
 * @details
 * - WHY A STRUCT AND NOT A `VkShaderModule`. The compilation half is what M2 learns and what a test can run without a
 *   device: glslang needs no Vulkan object, so "GLSL -> SPIR-V" is testable on its own (and the module, which does need
 *   a device, is created by the content path that owns the device).
 * - The words are the module's whole input: `vkCreateShaderModule` takes exactly this and nothing else.
 */
struct SpirvModule {
    /// The SPIR-V words, as `vkCreateShaderModule` consumes them (empty when nothing was compiled).
    std::vector<std::uint32_t> words;

    /**
     * @brief Answers whether the module holds no code.
     *
     * @return true when the words are empty.
     */
    [[nodiscard]] bool empty() const noexcept
    {
        return words.empty();
    }
};

/**
 * @brief Compiles ONE GLSL stage to SPIR-V with glslang's own front end.
 *
 * @details
 * - THE DIRECT PATH. The vsg backend reaches glslang through `vsg::ShaderCompiler`; this backend calls glslang itself,
 *   because it is the second implementation that exists to prove the seam does not need VulkanSceneGraph (see
 *   `.ai/research/graphics-2026-11-13-gpu-resource-abstraction.md` §8).
 * - WHAT THE TEXT MUST BE. GLSL for Vulkan: the stage is parsed with `EShMsgVulkanRules | EShMsgSpvRules` and the
 *   SPIR-V target `1.0`, which is what `glslangValidator -V` does. The engine's programs are written for exactly that
 *   ("GLSL, compiled at run time"), so no #define or dialect is added here (see the research note §9.4: source language
 *   is GLSL only for now, and the pipeline builder says so instead of guessing).
 * - THE OPTIMIZER IS OFF, and not because of a preference: this tree may build glslang with `ENABLE_OPT=OFF` (the vsg
 *   plugin's FetchContent settings do), so asking SPIRV-Tools to optimize would fail to link. Unoptimized SPIR-V is
 *   valid input to every Vulkan driver.
 * - PROCESS-WIDE INITIALIZATION. glslang requires `InitializeProcess()` before the first `TShader`; it runs once per
 *   process (a `std::once_flag`), because every compilation in every device shares it.
 *
 * @param stage The stage to compile (its source and entry point; an empty source is refused).
 * @param out   Receives the SPIR-V words on success, untouched on failure.
 * @param why   Receives glslang's own log (the reason phrase at the front) on failure, or an empty string on success.
 * @return true when the stage compiled and linked.
 */
[[nodiscard]] VN_BACKEND_VK_API bool compileStage(const vn::graphics::ShaderStage& stage, SpirvModule& out, vn::String& why);

VN_BACKEND_VK_NS_END
