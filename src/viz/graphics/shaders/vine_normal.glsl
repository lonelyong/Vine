// HOW A PLACEMENT TURNS A NORMAL: the inverse transpose of the model-view's upper 3x3, stated once for every vertex
// stage that hands a normal to the shading.
//
// WHY A FRAGMENT. Two built-in vertex stages transform an authored normal - the forward stage and the G-buffer writer -
// and both paths' shading reads the same view-space direction. Two copies of the transform is how the forward picture
// and the G-buffer would come to disagree about one placement, and the obvious spelling is wrong for a LEGAL
// placement: a model-view whose axes are scaled unevenly (a PositionAttitudeTransform's scale VECTOR, a shear left by
// a non-uniform parent scale, an instance matrix that carries one) turns a normal by the INVERSE TRANSPOSE. Multiplying
// the normal by the matrix itself keeps the direction only while the placement is rigid or uniformly scaled; under a
// non-uniform scale it sends the normal toward the stretched axis instead of away from it, and the surface is lit from
// a direction it does not have, with nothing reported (review 11 V2, review 18 Y8: the model view used to be fed
// through the matrix's upper 3x3 in both stages).
//
// WHY THE VERTEX STAGE OWNS IT. The alternative - a normal matrix computed on the CPU and packed into the draw block -
// cannot serve an INSTANCED draw, whose matrix is per instance and never reaches the per-draw packing. One rule on the
// vertex stage covers placed and instanced draws alike, and the cost is one 3x3 inverse per vertex of the two content
// stages (the depth and full-screen stages read no normal).
//
// THE DEGENERATE CASE. A placement with a zero scale on an axis collapses its subtree (see PositionAttitudeTransform),
// and the normal along the collapsed axis already had no defined direction: the matrix's upper 3x3 maps it to zero,
// the inverse transpose has no inverse to take. The picture of a collapsed draw is stated by the transform, not by
// this function.
//
// A CALLER PASSES THE COMBINED MODEL-VIEW - `vn_pc.model_view`, multiplied by the instance matrix when the draw is
// instanced - because the inverse transpose composes in the same order the transforms do.

/**
 * @brief Transforms an authored normal into view space by the inverse transpose of @p vn_model_view.
 *
 * @param vn_model_view Object -> view for the draw being recorded, column-major; the instance matrix already folded
 *                      in when the draw is instanced.
 * @param vn_normal Authored normal in object space; it does not have to be unit length.
 * @return The normal in view space, NOT normalised: the fragment stage normalises the interpolated value.
 */
vec3 vnTransformNormal(mat4 vn_model_view, vec3 vn_normal)
{
    return transpose(inverse(mat3(vn_model_view))) * vn_normal;
}
