#pragma once

#include <vine/robotics/robot_core_global.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

struct DHParameter {
    /*
    SDH
    - 连杆i的坐标系固定在连杆的远端；
    - 按照θ—>d—>α—>a 或者 d—>θ—>a—>α；
    - 对于树形结构或者闭链机构的机器人来说，按照SDH方法建立的连杆坐标系会产生歧义；
    */

    /*
    MDH
    - 连杆i的坐标系固定在连杆的近端；
    - 按照α—>a—>θ—>d
    */

    /*
     * @brief zᵢ₋₁ 与 zᵢ 的公法线长度
     * - MDH 沿 xᵢ₋₁ 平移 aᵢ₋₁
     * - SDH 沿 xᵢ 平移 aᵢ
     */
    double a{ 0. };

    /*
     * @brief 对齐Z轴（from old z axis to new z axis）
     * - MDH 绕 xᵢ₋₁ 旋转 αᵢ₋₁, 即zᵢ₋₁ → zᵢ 绕 xᵢ₋₁。
     * - SDH 绕 xᵢ 旋转 αᵢ。
     */
    double alpha{ 0. };

    /*
     * @brief z方向相偏移量
     * - MDH 沿 zᵢ 平移 dᵢ
     * - SDH 沿 zᵢ₋₁ 平移 dᵢ
     */
    double d{ 0. };

    /*
     * @brief 对齐X轴（from old x axis to new x axis）
     * - MDH 绕 zᵢ 旋转 θᵢ
     * - SDH 绕 zᵢ₋₁ 旋转 θᵢ
     */
    double theta{ 0. };
};

VN_ROBOTICS_KINEMATICS_NS_END