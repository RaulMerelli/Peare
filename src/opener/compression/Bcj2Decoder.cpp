#include "Bcj2Decoder.h"

#include "third_party/lzma/Bcj2.h"

#include <limits>

namespace peare {
namespace compression {

bool decodeBcj2(const std::array<const std::uint8_t*, 4>& streams,
                const std::array<std::size_t, 4>& sizes,
                std::size_t unpackedSize, std::vector<std::uint8_t>* output)
{
    if (!output || (sizes[1] & 3) != 0 || (sizes[2] & 3) != 0)
        return false;
    if (sizes[0] > std::numeric_limits<std::size_t>::max() - sizes[1] ||
        sizes[0] + sizes[1] > std::numeric_limits<std::size_t>::max() - sizes[2] ||
        sizes[0] + sizes[1] + sizes[2] != unpackedSize)
        return false;

    static const std::uint8_t empty = 0;
    std::vector<std::uint8_t> decoded(unpackedSize);
    CBcj2Dec state = {};
    Bcj2Dec_Init(&state);
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const std::uint8_t* begin = streams[i] ? streams[i] : &empty;
        if (sizes[i] && !streams[i]) return false;
        state.bufs[i] = begin;
        state.lims[i] = begin + sizes[i];
    }
    state.dest = decoded.empty() ? const_cast<Byte*>(&empty) : decoded.data();
    state.destLim = state.dest + decoded.size();

    if (Bcj2Dec_Decode(&state) != SZ_OK ||
        state.dest != state.destLim || !Bcj2Dec_IsMaybeFinished(&state))
        return false;
    for (std::size_t i = 0; i < streams.size(); ++i)
        if (state.bufs[i] != state.lims[i]) return false;
    output->swap(decoded);
    return true;
}

}  // namespace compression
}  // namespace peare
