#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <vine/math/Matrix4x4.hpp>
#include <vine/vsg/api/ProgramAbi.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The camera matrices the ENGINE's push block declares, filled BY NAME from the pass' camera and the
 * drawable's model matrix.
 *
 * WHY THE PUSH EXISTS AT ALL. On this backend the content ABI's blocks are L1 structs bound at whatever
 * `(set, binding)` a program declares (see api/ProgramAbi and `.ai/design/vsg-reimplementation.md` §11.16af)
 * - but the engine's own programs also declare a 128-byte push range, and that range is their L2 realization
 * of the SAME L1 values:
 *
 *   pc.projection == VineViewBlock.proj          (the pass' projection, in the DEVICE's clip convention)
 *   pc.modelView  == VineViewBlock.view * VineDrawBlock.model
 *
 * The old backend's note spells it out (`VsgPipelineFactory.cpp`, "The L1 contract is the SDK's
 * VineViewBlock/VineDrawBlock ... on this backend the 128-byte push range is their L2 realization"): the
 * same two matrices a block-reading program finds in the view block are, for a push-reading program, in a
 * push. Filling it is therefore not a new convention - it is the same fold and the same composition, spelled
 * for the range the text declares.
 *
 * BY NAME, NEVER BY POSITION. A push range is the one range whose bytes are ASSEMBLED rather than copied
 * from an L1 struct, so "which member is which" can only come from the names the text wrote. Two rules
 * follow, and both are refusals rather than zero-fills:
 *
 *   * a member name this file does not know is refused, with the name in the message - a program that reads
 *     `pc.color` would otherwise shade with zeros, and "the picture is black because the bytes were zero" is
 *     not a diagnosis;
 *   * a known name of the WRONG SIZE is refused too (`vec4 projection` is not a projection matrix) - the
 *     value would be written into bytes that are not its own.
 *
 * WHICH STAGE reads a range is a fact of the text (`AbiPushRange::stages`), and the range may be the fragment
 * stage's (the deferred lighting programs read their lights through a fragment push). This file only fills
 * bytes: the range's offset, its size and its stages come from the declaration, and the serving layer hands
 * the result to `PushConstants` as declared.
 *
 * A SNAPSHOT WITH NO CAMERA writes zeros, exactly as buildViewBlock does (api/ViewBlock.hpp): "there is no
 * view" is a value, not an error, and the caller that has no camera decides what to do about it. The members
 * are still considered filled - the range is written, defined, and zero.
 *
 * @see api/ViewBlock.hpp (the fold and the block the same values may live in)
 * @see api/ProgramAbi.hpp (`AbiPushRange::members`: the declarations this fills)
 */
VN_VSG_NS_BEGIN

/** @brief Which L1 value a declared push member names. */
enum class ContentPushMember : std::uint8_t
{
    Unknown,     ///< Not part of the L2 realization of the camera pair: refused by name, never zero-filled.
    Projection,  ///< `projection`: the pass' projection, folded into the device's clip convention.
    ModelView,   ///< `modelView`: the pass' view matrix times the DRAWABLE's model matrix.
};

/**
 * @brief Reads one declared member name as the L1 value it asks for.
 *
 * @param member_name The name the text declared (`projection`, `modelView`, ...).
 * @return Which value it is, or ContentPushMember::Unknown when it is none of them.
 */
[[nodiscard]] ContentPushMember contentPushMemberOf(std::string_view member_name) noexcept;

/**
 * @brief The declared name a value would be filled from (for diagnostics and test expectations).
 *
 * @param member Which value.
 * @return The name that fills it (`"projection"`, `"modelView"`; `"?"` for Unknown).
 */
[[nodiscard]] const char* contentPushMemberName(ContentPushMember member) noexcept;

/**
 * @brief Gets whether a declared member is a value this backend can put into a push range.
 *
 * The size is part of the answer: a known name declares a `mat4` (64 bytes), and any other size is a
 * declaration whose bytes are not that value's.
 *
 * @param member One declared member.
 * @return true when the packer can fill it.
 */
[[nodiscard]] bool canFillPushMember(const AbiPushMember& member) noexcept;

/**
 * @brief The API's stage flags for a declared push range's stages (a `VkShaderStageFlags` value).
 *
 * One spelling for both sides of the seam: the layer that declares the range in its pipeline layout and the
 * pass that writes it must name the same stages, and a push written for a stage the layout does not declare
 * it for is a validation error (and, on drivers that tolerate it, a push the shader never sees).
 *
 * @param stages The declared stages (AbiStage bits).
 * @return The matching stage flags.
 */
[[nodiscard]] std::uint32_t pushStagesOf(std::uint32_t stages) noexcept;

/**
 * @brief Fills one declared push range from the pass' camera and the drawable's model matrix.
 *
 * Members are filled by NAME (see the file note); every byte outside them stays zero, so the range is fully
 * written and defined even when the text declares fewer members than the pipeline layout's range covers.
 *
 * @param range     The declared range (its members are what is filled).
 * @param camera    The pass' camera snapshot (a snapshot with no camera writes zeros).
 * @param model     The drawable's model matrix (object -> world).
 * @param out       Receives the range's bytes, `range.size` of them.
 * @param unhandled Receives the name of the first member that cannot be filled (empty when the call returns
 *                  true, and also when the range itself is unusable - see the return value).
 * @return true when the range was filled; false when it must be refused (an unfillable member, or a range
 *         with no size at all).
 */
[[nodiscard]] bool packContentPush(const AbiPushRange& range, const core::CameraSnapshot& camera,
                                   const vn::math::Mat4d& model, std::vector<std::byte>& out,
                                   std::string_view& unhandled);

VN_VSG_NS_END
