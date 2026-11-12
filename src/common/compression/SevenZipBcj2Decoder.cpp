#include "SevenZipBcj2Decoder.h"

#include "../../opener/compression/third_party/lzma/Bcj2.h"

#include <limits>

namespace peare {
namespace compression {

bool decodeSevenZipBcj2(const std::array<const std::uint8_t*, 4>& streams,
                        const std::array<std::size_t, 4>& sizes,
                        std::size_t unpackedSize,
                        std::vector<std::uint8_t>* output)
{
    if (!output || (sizes[1] & 3U) || (sizes[2] & 3U)) return false;
    if (sizes[0] > (std::numeric_limits<std::size_t>::max)() - sizes[1] ||
        sizes[0] + sizes[1] > (std::numeric_limits<std::size_t>::max)() - sizes[2] ||
        sizes[0] + sizes[1] + sizes[2] != unpackedSize)
        return false;

    static const std::uint8_t empty = 0;
    std::vector<std::uint8_t> decoded;
    try {
        decoded.resize(unpackedSize);
    } catch (...) {
        return false;
    }
    CBcj2Dec state = {};
    Bcj2Dec_Init(&state);
    for (std::size_t index = 0; index < streams.size(); ++index) {
        if (sizes[index] && !streams[index]) return false;
        const std::uint8_t* begin = streams[index] ? streams[index] : &empty;
        state.bufs[index] = begin;
        state.lims[index] = begin + sizes[index];
    }
    state.dest = decoded.empty() ? const_cast<Byte*>(&empty) : decoded.data();
    state.destLim = state.dest + decoded.size();
    if (Bcj2Dec_Decode(&state) != SZ_OK || state.dest != state.destLim ||
        !Bcj2Dec_IsMaybeFinished(&state))
        return false;
    for (std::size_t index = 0; index < streams.size(); ++index)
        if (state.bufs[index] != state.lims[index]) return false;
    output->swap(decoded);
    return true;
}

}  // namespace compression
}  // namespace peare
