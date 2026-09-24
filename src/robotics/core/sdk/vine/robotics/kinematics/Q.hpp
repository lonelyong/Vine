#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include <vine/SmallVector.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

/**
 * @brief Joint-space configuration vector.
 *
 * Wraps an inline-capacity double array (SmallVector<double, 8>) and adds the
 * vector arithmetic used by kinematics solvers: dot product, element-wise
 * operations, concatenation, sub-vectors, norms and scalar scaling.
 */
class VN_ROBOTICS_CORE_API Q : public vn::SmallVector<double, 8> {
  public:
    using MyBase = vn::SmallVector<double, 8>;

  public:
    /** @brief Constructs an empty joint vector. */
    Q() = default;

    /**
     * @brief Constructs a zero joint vector of the given length.
     *
     * @param size The number of joints.
     */
    explicit Q(std::size_t size)
      : MyBase(size)
    {}

    /**
     * @brief Constructs a joint vector from a raw double array.
     *
     * @param values The input values.
     * @param size The number of values.
     */
    Q(const double* values, std::size_t size)
    {
        MyBase::reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            MyBase::push_back(values[i]);
        }
    }

    /**
     * @brief Constructs a joint vector from an initializer list.
     *
     * @param values The input values.
     */
    Q(std::initializer_list<double> values)
      : MyBase(values)
    {}

    /**
     * @brief Constructs a joint vector from an std::vector.
     *
     * @param values The input values.
     */
    explicit Q(const std::vector<double>& values)
    {
        MyBase::reserve(values.size());
        for (double value : values) {
            MyBase::push_back(value);
        }
    }

  public:
    /**
     * @brief Computes the dot product with another joint vector.
     *
     * @param right The other vector, must have the same size.
     * @return The dot product.
     * @throws std::logic_error when the sizes differ.
     */
    double dot(const Q& right) const
    {
        checkSize(*this, right, "Q::dot");
        double sum = 0.0;
        for (std::size_t i = 0; i < size(); ++i) {
            sum += (*this)[i] * right[i];
        }
        return sum;
    }

    /**
     * @brief Computes the element-wise product with another joint vector.
     *
     * @param right The other vector, must have the same size.
     * @return The element-wise product.
     * @throws std::logic_error when the sizes differ.
     */
    Q elemMul(const Q& right) const
    {
        checkSize(*this, right, "Q::elemMul");
        Q result(size());
        for (std::size_t i = 0; i < size(); ++i) {
            result[i] = (*this)[i] * right[i];
        }
        return result;
    }

    /**
     * @brief Concatenates another joint vector after this one.
     *
     * @param right The vector to append.
     * @return The concatenated vector; its length is the sum of both.
     */
    Q concat(const Q& right) const noexcept
    {
        Q result(*this);
        result.append(right);
        return result;
    }

    /**
     * @brief Appends a single joint value.
     *
     * @param value The value to append.
     * @return *this, for chaining.
     */
    Q& append(double value) noexcept
    {
        push_back(value);
        return *this;
    }

    /**
     * @brief Appends all values of another joint vector.
     *
     * @param other The vector to append.
     * @return *this, for chaining.
     */
    Q& append(const Q& other) noexcept
    {
        reserve(size() + other.size());
        for (double value : other) {
            push_back(value);
        }
        return *this;
    }

    /**
     * @brief Returns a sub-vector of this joint vector.
     *
     * @param offset The starting offset.
     * @param count The number of values.
     * @return The extracted sub-vector.
     * @throws std::out_of_range when offset + count exceeds the size.
     */
    Q subQ(std::size_t offset, std::size_t count) const
    {
        if (count > size() || offset > size() - count) {
            throw std::out_of_range("Q::subQ, invalid parameters");
        }
        Q result(count);
        for (std::size_t i = 0; i < count; ++i) {
            result[i] = (*this)[offset + i];
        }
        return result;
    }

    /**
     * @brief Writes consecutive values starting at an offset.
     *
     * @param offset The starting offset.
     * @param values The values to write.
     * @param count The number of values.
     * @throws std::out_of_range when offset + count exceeds the size.
     */
    void set(std::size_t offset, const double* values, std::size_t count)
    {
        if (count > size() || offset > size() - count) {
            throw std::out_of_range("Q::set, invalid parameters");
        }
        for (std::size_t i = 0; i < count; ++i) {
            (*this)[offset + i] = values[i];
        }
    }

    /**
     * @brief Returns the Euclidean norm of the vector.
     *
     * @return The norm.
     */
    double norm() const noexcept
    {
        return std::sqrt(normSquared());
    }

    /**
     * @brief Returns the squared Euclidean norm of the vector.
     *
     * @return The squared norm.
     */
    double normSquared() const noexcept
    {
        double sum = 0.0;
        for (double value : *this) {
            sum += value * value;
        }
        return sum;
    }

    /**
     * @brief Copies the joint values into an std::vector.
     *
     * @return The copied values.
     */
    std::vector<double> toStdVector() const
    {
        return std::vector<double>(begin(), end());
    }

    /**
     * @brief Compares two joint vectors element-wise.
     *
     * @param right The other vector.
     * @return true when the sizes and all values match.
     */
    bool operator==(const Q& right) const noexcept
    {
        if (size() != right.size()) {
            return false;
        }
        for (std::size_t i = 0; i < size(); ++i) {
            if ((*this)[i] != right[i]) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Compares two joint vectors for inequality.
     *
     * @param right The other vector.
     * @return true when they differ.
     */
    bool operator!=(const Q& right) const noexcept
    {
        return !(*this == right);
    }

    /**
     * @brief Adds another joint vector element-wise.
     *
     * @param right The other vector, must have the same size.
     * @return The element-wise sum.
     * @throws std::logic_error when the sizes differ.
     */
    Q operator+(const Q& right) const
    {
        checkSize(*this, right, "Q::operator+");
        Q result(size());
        for (std::size_t i = 0; i < size(); ++i) {
            result[i] = (*this)[i] + right[i];
        }
        return result;
    }

    /**
     * @brief Adds another joint vector in place.
     *
     * @param right The other vector, must have the same size.
     * @return *this.
     * @throws std::logic_error when the sizes differ.
     */
    Q& operator+=(const Q& right)
    {
        checkSize(*this, right, "Q::operator+=");
        for (std::size_t i = 0; i < size(); ++i) {
            (*this)[i] += right[i];
        }
        return *this;
    }

    /**
     * @brief Subtracts another joint vector element-wise.
     *
     * @param right The other vector, must have the same size.
     * @return The element-wise difference.
     * @throws std::logic_error when the sizes differ.
     */
    Q operator-(const Q& right) const
    {
        checkSize(*this, right, "Q::operator-");
        Q result(size());
        for (std::size_t i = 0; i < size(); ++i) {
            result[i] = (*this)[i] - right[i];
        }
        return result;
    }

    /**
     * @brief Subtracts another joint vector in place.
     *
     * @param right The other vector, must have the same size.
     * @return *this.
     * @throws std::logic_error when the sizes differ.
     */
    Q& operator-=(const Q& right)
    {
        checkSize(*this, right, "Q::operator-=");
        for (std::size_t i = 0; i < size(); ++i) {
            (*this)[i] -= right[i];
        }
        return *this;
    }

    /**
     * @brief Multiplies every value by a scalar.
     *
     * @param scale The scale factor.
     * @return The scaled vector.
     */
    Q operator*(double scale) const noexcept
    {
        Q result(size());
        for (std::size_t i = 0; i < size(); ++i) {
            result[i] = (*this)[i] * scale;
        }
        return result;
    }

    /**
     * @brief Multiplies every value by a scalar in place.
     *
     * @param scale The scale factor.
     * @return *this.
     */
    Q& operator*=(double scale) noexcept
    {
        for (std::size_t i = 0; i < size(); ++i) {
            (*this)[i] *= scale;
        }
        return *this;
    }

    /**
     * @brief Divides every value by a scalar.
     *
     * @param scale The divisor.
     * @return The scaled vector.
     */
    Q operator/(double scale) const noexcept
    {
        Q result(size());
        for (std::size_t i = 0; i < size(); ++i) {
            result[i] = (*this)[i] / scale;
        }
        return result;
    }

    /**
     * @brief Divides every value by a scalar in place.
     *
     * @param scale The divisor.
     * @return *this.
     */
    Q& operator/=(double scale) noexcept
    {
        for (std::size_t i = 0; i < size(); ++i) {
            (*this)[i] /= scale;
        }
        return *this;
    }

  private:
    /**
     * @brief Throws when two joint vectors have different sizes.
     *
     * @param left The left operand.
     * @param right The right operand.
     * @param operation The operation name used in the error message.
     * @throws std::logic_error when the sizes differ.
     */
    static void checkSize(const Q& left, const Q& right, const char* operation)
    {
        if (left.size() != right.size()) {
            throw std::logic_error(std::string(operation) + ": size mismatch");
        }
    }
};

VN_ROBOTICS_KINEMATICS_NS_END