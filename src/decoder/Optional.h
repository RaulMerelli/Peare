#pragma once

#include <utility>

namespace peare {

template <typename T>
class Optional
{
public:
    Optional() : m_hasValue(false), m_value() {}
    Optional(const T& value) : m_hasValue(true), m_value(value) {}
    Optional(T&& value) : m_hasValue(true), m_value(std::move(value)) {}

    Optional(const Optional& other)
        : m_hasValue(other.m_hasValue), m_value(other.m_value) {}

    Optional(Optional&& other)
        : m_hasValue(other.m_hasValue), m_value(std::move(other.m_value)) {}

    Optional& operator=(const Optional& other)
    {
        if (this != &other) {
            m_hasValue = other.m_hasValue;
            m_value = other.m_value;
        }
        return *this;
    }

    Optional& operator=(Optional&& other)
    {
        if (this != &other) {
            m_hasValue = other.m_hasValue;
            m_value = std::move(other.m_value);
        }
        return *this;
    }

    Optional& operator=(const T& value)
    {
        m_value = value;
        m_hasValue = true;
        return *this;
    }

    Optional& operator=(T&& value)
    {
        m_value = std::move(value);
        m_hasValue = true;
        return *this;
    }

    bool has_value() const { return m_hasValue; }
    explicit operator bool() const { return m_hasValue; }

    T& value() { return m_value; }
    const T& value() const { return m_value; }

    T value_or(const T& fallback) const
    {
        return m_hasValue ? m_value : fallback;
    }

    T& operator*() { return m_value; }
    const T& operator*() const { return m_value; }

    T* operator->() { return &m_value; }
    const T* operator->() const { return &m_value; }

    void reset() { m_hasValue = false; }

private:
    bool m_hasValue;
    T m_value;
};

} // namespace peare
