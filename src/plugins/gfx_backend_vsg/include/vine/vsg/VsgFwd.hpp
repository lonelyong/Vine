#pragma once

/**
 * @brief The vsg types this backend only ever names behind a pointer or a ref_ptr.
 *
 * WHY THIS FILE EXISTS. Compiling `vsg/app/Viewer.h` costs about a second by itself (measured: 1.06 s /
 * 208 MB peak, and its siblings are not far behind — View.h 0.76, RenderGraph.h 0.76, ShaderSet.h 0.73,
 * CommandGraph.h 0.69, PipelineBarrier.h 0.64), and whichever of them a header includes, EVERY translation
 * unit that reaches that header pays it again. The plugin's headers reached them for no more than a name:
 * a member of type `::vsg::ref_ptr<T>` does not need T to be complete, because the reference counting
 * happens in the translation unit that destroys the object — which is exactly why the classes holding those
 * members declare their destructors out of line (see VsgRendererState and VsgRenderer, whose destructors
 * live in VsgRenderer.cpp).
 *
 * THE RULE, so this list does not quietly become a second interface: a type belongs here only while EVERY
 * header that names it needs nothing more than a pointer or a reference. The moment one of them calls a
 * method, takes its size, or holds it by value, the type moves back to a real include in THAT header — not
 * to this file — and the guarantee this file makes stays true for the rest.
 *
 * WHAT IT IS NOT: the backend's own types. This is only the vendored library's boundary, and only for what
 * the headers name; the sources include what they use, as they always did.
 */

namespace vsg
{

class CommandGraph;
class ResourceHints;
class ShaderSet;
class View;
class Viewer;
class Window;

}  // namespace vsg
