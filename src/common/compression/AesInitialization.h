#pragma once

namespace peare {
namespace compression {

#if !defined(_WIN32) && defined(__GNUC__)
__attribute__((visibility("hidden")))
#endif
void initializeAesTables();

} // namespace compression
} // namespace peare
