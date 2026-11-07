#include "PackMp3Decoder.h"

#include "third_party/packmp3/packmp3lib.h"
#include "third_party/packmp3/bitops.h"

#include <climits>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>

namespace peare {
namespace compression {
namespace {

std::mutex decoderMutex;

}

bool decodePackMp3(const std::uint8_t* input, std::size_t inputSize,
                   std::size_t outputLimit, std::vector<std::uint8_t>* output) {
    if (input == nullptr || output == nullptr || inputSize < 3 ||
        inputSize > static_cast<std::size_t>(INT_MAX) || outputLimit == 0 ||
        outputLimit > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(decoderMutex);
    iostream::set_memory_write_limit(static_cast<int>(outputLimit));
    pmplib_init_streams(const_cast<std::uint8_t*>(input), 1,
                        static_cast<int>(inputSize), nullptr, 1);
    unsigned char* decoded = nullptr;
    unsigned int decodedSize = 0;
    const bool valid = pmplib_convert_stream2mem(&decoded, &decodedSize, nullptr);
    iostream::set_memory_write_limit(-1);
    std::unique_ptr<unsigned char, decltype(&std::free)> decodedOwner(decoded, &std::free);
    if (!valid || decoded == nullptr || decodedSize != outputLimit) {
        return false;
    }

    std::vector<std::uint8_t> result;
    try {
        result.assign(decoded, decoded + decodedSize);
    } catch (const std::bad_alloc&) {
        return false;
    }
    output->swap(result);
    return true;
}

}
}
