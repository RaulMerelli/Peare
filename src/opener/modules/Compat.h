#pragma once

#include <memory>
#include <utility>

namespace peare {

template <typename T, typename... Args>
std::unique_ptr<T> makeUnique(Args&&... args)
{
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

} // namespace peare
