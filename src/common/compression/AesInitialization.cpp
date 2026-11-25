#include "AesInitialization.h"

#include "../../opener/compression/third_party/lzma/Aes.h"

#include <mutex>

namespace peare {
namespace compression {

void initializeAesTables() {
    static std::once_flag once;
    std::call_once(once, AesGenTables);
}

} // namespace compression
} // namespace peare
