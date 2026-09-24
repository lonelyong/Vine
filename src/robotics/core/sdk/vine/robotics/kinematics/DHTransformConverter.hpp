#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <optional>

#include <vine/math/Isometry3.hpp>
#include <vine/robotics/kinematics/DHParameter.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

/* ========================================================================= */
/*  Forward transforms (always succeed)                                      */
/* ========================================================================= */

/*
 * MDH: T = Rot_x(α) · Trans_x(a) · Rot_z(θ + dtheta) · Trans_z(d + dd)
 *
 * @param mdh     Nominal MDH parameters.
 * @param dd      Offset added to d  (prismatic joint variable).
 * @param dtheta  Offset added to θ (revolute joint variable).
 */
VN_ROBOTICS_CORE_API math::Isometry3d mdhToTransform(const DHParameter& mdh,
                                double dd = 0.0, double dtheta = 0.0);

/*
 * SDH: T = Rot_z(θ + dtheta) · Trans_z(d + dd) · Rot_x(α) · Trans_x(a)
 *
 * @param sdh     Nominal SDH parameters.
 * @param dd      Offset added to d  (prismatic joint variable).
 * @param dtheta  Offset added to θ (revolute joint variable).
 */
VN_ROBOTICS_CORE_API math::Isometry3d sdhToTransform(const DHParameter& sdh,
                                double dd = 0.0, double dtheta = 0.0);

/* ========================================================================= */
/*  Representability checks                                                  */
/* ========================================================================= */

/*
 * Check whether the rotation satisfies the MDH structural constraint:
 *
 *   R = Rot_x(α) · Rot_z(θ)  ⇔  R(0,2) = 0
 *
 * (element at row 0, column 2 of the 3×3 rotation matrix).
 */
VN_ROBOTICS_CORE_API bool isMdhRepresentable(const math::Isometry3d& tf, double tolerance = 1e-10);

/*
 * Check whether the rotation matrix satisfies the SDH structural constraint:
 *
 *   R = Rot_z(θ) · Rot_x(α)  ⇔  R(2,0) = 0
 *
 * (element at row 2, column 0 of the 3×3 rotation matrix).
 */
VN_ROBOTICS_CORE_API bool isSdhRepresentable(const math::Isometry3d& tf, double tolerance = 1e-10);

/* ========================================================================= */
/*  Inverse extraction (may fail → std::optional)                            */
/* ========================================================================= */

/*
 * Try to extract MDH parameters from a transform.
 *
 * Preconditions (checked internally):
 *   - Quaternion is unit (|q|² ≈ 1)
 *   - isMdhRepresentable(tf, tolerance)
 *
 * Returns std::nullopt if either precondition fails.
 *
 * MDH:  T = Rot_x(α) · Trans_x(a) · Rot_z(θ) · Trans_z(d)
 */
VN_ROBOTICS_CORE_API std::optional<DHParameter> tryMdhFromTransform(const math::Isometry3d& tf,
                                                double tolerance = 1e-10);

/*
 * Try to extract SDH parameters from a transform.
 *
 * Preconditions (checked internally):
 *   - Quaternion is unit (|q|² ≈ 1)
 *   - isSdhRepresentable(tf, tolerance)
 *
 * Returns std::nullopt if either precondition fails.
 *
 * SDH:  T = Rot_z(θ) · Trans_z(d) · Rot_x(α) · Trans_x(a)
 */
VN_ROBOTICS_CORE_API std::optional<DHParameter> trySdhFromTransform(const math::Isometry3d& tf,
                                                double tolerance = 1e-10);

VN_ROBOTICS_KINEMATICS_NS_END