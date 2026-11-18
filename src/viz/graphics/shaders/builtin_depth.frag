#version 450
// THE FRAGMENT STAGE OF A DEPTH-ONLY PASS: it declares no output at all.
//
// WHY IT IS EMPTY. A pipeline that rasterises needs a fragment stage (rasterizerDiscardEnable is the only way to go
// without one, and it also stops depth writes), while a shadow map has no colour attachment for that stage to write.
// A stage that still writes location 0 there is a validation warning that says exactly this: the write is unused and
// the text is wrong about what the pass does. Declaring nothing is the truthful version of "this pass writes depth".
//
// THE VERTEX STAGE IS THE CONTENT'S OWN (see BuiltinShaders::depthOnlyProgram): same push constants, same instancing,
// same per-drawable position expression as the colour passes, so a triangle's depth here and its colour pass land in
// the same place by construction rather than by a second copy of the transform.
void main()
{
}
