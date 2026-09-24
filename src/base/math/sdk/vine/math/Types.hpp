#pragma once

#include "math_global.hpp"

#include <concepts>
#include <cstdint>
#include <type_traits>

VN_MATH_NS_BEGIN

/**
 * @brief floating point concept, includes float and double
 */
template <typename T>
concept FloatingPoint = std::is_floating_point<T>::value;

template <typename T>
concept FP = std::is_floating_point<T>::value;

/**
 * @brief Integral concept, includes all integral types (signed and unsigned and boolean)
 */
template <typename T>
concept Integral = std::integral<T>;

/**
 * @brief Interger concept, includes all integer types (signed and unsigned) but not boolean
 */
template <typename T>
concept Integer = std::is_same<T, int8_t>::value || std::is_same<T, uint8_t>::value || std::is_same<T, int16_t>::value || std::is_same<T, uint16_t>::value ||
                  std::is_same<T, int32_t>::value || std::is_same<T, uint32_t>::value || std::is_same<T, int64_t>::value || std::is_same<T, uint64_t>::value;

/**
 * @brief Real concept, includes all floating point and integer types (but not boolean)
 */
template <typename T>
concept Real = FP<T> || Integer<T>;

/**
 * @brief Arithmetic concept, includes all floating point and integral types (include boolean)
 */
template <typename T>
concept Arithmetic = FP<T> || Integral<T>;

/**
 * @brief double for integral types, and T itself for floating point types
 */
template <Arithmetic T>
using TypeF = std::conditional_t<std::is_integral_v<T>, double, T>;

/**
 * @brief Tag type for column-major storage order (default).
 *
 * Elements are stored column by column: m(row, col) = data[col * rows + row].
 * Column vectors are contiguous in memory — optimal for matrix-vector multiply.
 */
struct ColMajor {};

/**
 * @brief Tag type for row-major storage order.
 *
 * Elements are stored row by row: m(row, col) = data[row * cols + col].
 * Row vectors are contiguous in memory — convenient for row-wise iteration
 * and interop with libraries that expect row-major layout.
 */
struct RowMajor {};

VN_MATH_NS_END
