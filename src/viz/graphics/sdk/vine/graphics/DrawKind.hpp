#pragma once
#include "graphics_global.hpp"

#include <cstdint>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Which of the engine's TWO drawings a pass, a draw packet or a compiled pipeline is.
 *
 * @details
 * - ONE FACT, THREE PLACES. The engine knows exactly two ways to put pixels on a target, and the same value
 *   names it wherever it is read: what a pass IS (`PassDesc::kind`), what one packet of that pass issues
 *   (`DrawPacket::kind`), and which descriptor ABI the pipeline recording it is compiled against
 *   (`PipelineKey::kind`). One enumeration for all three, so a pipeline cannot be compiled for one kind and
 *   bound for the other.
 * - WHY THE KIND IS IDENTITY. The two are not interchangeable compilations of one program: a scene draw binds
 *   the four ABI blocks at set 0 and its sampled inputs at set 1, while a full-screen draw binds the source's
 *   colour attachments at bindings 0..N-1 of set 0 (the full-screen ABI the engine's own screen-space
 *   programs, `BuiltinShaders::fullscreenCopyProgram` and friends, are written against). A pipeline compiled for
 *   one of them cannot be bound for the other.
 * - THE VALUE NAMES THE DRAW, NOT THE TARGET. `Fullscreen` means a full-screen triangle: the draw covers the
 *   VIEW it is given, wherever that view lands - a picture-in-picture rectangle, an off-screen image, or a
 *   window. "Screen" is the window in this module (see `RenderArea` / `PresentTarget`), which is why the kind
 *   does not use that word: the presenting pass is itself one of these.
 * - WHY THE OTHER VALUE IS `Scene` AND NOT `Content`. A scene pass draws more than content geometry - lines,
 *   impostors, gizmos and overlays are all scene draws - and `Content` would read as "anything the engine
 *   owns", which includes the full-screen programs.
 *
 * @note
 * - ADDING A VALUE IS A DESIGN BATCH, NOT A WORD SWEEP. The values are compared by name
 *   (`kind == DrawKind::Fullscreen`) where they branch, so there is no `switch` for the compiler to check and a new
 *   value is FOUND rather than flagged: `grep -rn 'DrawKind::' src` lists every site, and
 *   `graphics-architecture.md` 2.11's pass-kind row says what each of them must answer.
 */
enum class DrawKind : std::uint8_t
{
    Scene,      // Draw collected geometry through a camera: the ordinary pass (opaque, G-buffer, shadow, HUD).
    Fullscreen, // Draw a full-screen triangle through a program over its resolved inputs (composite, lighting).
};

VN_GRAPHICS_NS_END
