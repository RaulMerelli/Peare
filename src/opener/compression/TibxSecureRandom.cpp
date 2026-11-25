#include "TibxSecureRandom.h"

#include <algorithm>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace peare {
namespace fs {

bool tibxSecureRandom(std::uint8_t* data, std::size_t size) {
    if (!data && size) return false;
#if defined(_WIN32)
    while (size) {
        const ULONG count = static_cast<ULONG>(std::min<std::size_t>(
            size, (std::numeric_limits<ULONG>::max)()));
        if (BCryptGenRandom(nullptr, data, count, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            return false;
        data += count;
        size -= count;
    }
    return true;
#elif defined(__linux__)
#if defined(SYS_getrandom)
    while (size) {
        const std::size_t count = std::min<std::size_t>(size, 256);
        const ssize_t result = ::syscall(SYS_getrandom, data, count, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        data += result;
        size -= static_cast<std::size_t>(result);
    }
    return true;
#else
    return size == 0;
#endif
#elif defined(__unix__) || defined(__APPLE__)
    if (!size) return true;
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd;
    do { fd = ::open("/dev/urandom", flags); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return false;
    bool success = true;
    while (size) {
        const std::size_t count = std::min<std::size_t>(
            size, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t result = ::read(fd, data, count);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) { success = false; break; }
        data += result;
        size -= static_cast<std::size_t>(result);
    }
    ::close(fd);
    return success;
#else
    return size == 0;
#endif
}

} // namespace fs
} // namespace peare
