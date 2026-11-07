#include "WavPackDecoder.h"

#include "wavpack.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

namespace peare {
namespace compression {
namespace {

struct MemoryReader {
    const std::uint8_t* data = nullptr;
    std::int64_t size = 0;
    std::int64_t position = 0;
};

std::int32_t readBytes(void* id, void* output, std::int32_t count)
{
    auto* reader = static_cast<MemoryReader*>(id);
    if (!reader || !output || count <= 0 || reader->position < 0 ||
        reader->position > reader->size) return 0;
    const auto available = reader->size - reader->position;
    const auto amount = std::min<std::int64_t>(available, count);
    if (amount <= 0) return 0;
    std::memcpy(output, reader->data + reader->position, std::size_t(amount));
    reader->position += amount;
    return std::int32_t(amount);
}

std::int64_t getPosition(void* id)
{
    return static_cast<MemoryReader*>(id)->position;
}

int setPosition(void* id, std::int64_t position)
{
    auto* reader = static_cast<MemoryReader*>(id);
    if (!reader || position < 0 || position > reader->size) return -1;
    reader->position = position;
    return 0;
}

int setRelativePosition(void* id, std::int64_t delta, int origin)
{
    auto* reader = static_cast<MemoryReader*>(id);
    if (!reader) return -1;
    std::int64_t base = 0;
    if (origin == SEEK_CUR) base = reader->position;
    else if (origin == SEEK_END) base = reader->size;
    else if (origin != SEEK_SET) return -1;
    if ((delta > 0 && base > std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && base < std::numeric_limits<std::int64_t>::min() - delta)) return -1;
    return setPosition(id, base + delta);
}

int pushBackByte(void* id, int value)
{
    auto* reader = static_cast<MemoryReader*>(id);
    if (!reader || reader->position <= 0 || reader->position > reader->size ||
        reader->data[reader->position - 1] != std::uint8_t(value)) return EOF;
    --reader->position;
    return value;
}

std::int64_t getLength(void* id)
{
    return static_cast<MemoryReader*>(id)->size;
}

int canSeek(void*)
{
    return 1;
}

int closeReader(void*)
{
    return 0;
}

const WavpackStreamReader64 kReader = {
    readBytes,
    nullptr,
    getPosition,
    setPosition,
    setRelativePosition,
    pushBackByte,
    getLength,
    canSeek,
    nullptr,
    closeReader
};

bool appendWrapper(WavpackContext* context, std::vector<std::uint8_t>* output)
{
    const std::uint32_t size = WavpackGetWrapperBytes(context);
    const auto* data = WavpackGetWrapperData(context);
    if (!size) return true;
    if (size && !data) return false;
    if (size > output->max_size() - output->size()) return false;
    output->insert(output->end(), data, data + size);
    return true;
}

} // namespace

bool decodeWavPackZip(const std::uint8_t* data, std::size_t size,
                      std::size_t expectedSize, std::vector<std::uint8_t>* output)
{
    if (!data || !size || !output || size > std::size_t(std::numeric_limits<std::int64_t>::max()))
        return false;

    try {
        MemoryReader reader{data, std::int64_t(size), 0};
        char error[128] = {};
        std::unique_ptr<WavpackContext, decltype(&WavpackCloseFile)> context(
            WavpackOpenFileInputEx64(const_cast<WavpackStreamReader64*>(&kReader),
                &reader, nullptr, error, OPEN_WRAPPER, 0), WavpackCloseFile);
        if (!context) return false;
        if (!(WavpackGetMode(context.get()) & MODE_LOSSLESS) ||
            !WavpackGetWrapperBytes(context.get())) return false;

        const std::int64_t sampleCount = WavpackGetNumSamples64(context.get());
        const int channels = WavpackGetNumChannels(context.get());
        const int bytesPerSample = WavpackGetBytesPerSample(context.get());
        if (sampleCount < 0 || channels <= 0 || channels > 256 ||
            bytesPerSample <= 0 || bytesPerSample > 4 ||
            std::uint64_t(sampleCount) > std::numeric_limits<std::size_t>::max() /
                std::size_t(channels) / std::size_t(bytesPerSample)) return false;

        const std::size_t audioSize = std::size_t(sampleCount) * std::size_t(channels) *
            std::size_t(bytesPerSample);
        output->clear();
        if (!appendWrapper(context.get(), output)) return false;
        WavpackFreeWrapper(context.get());
        if (audioSize > output->max_size() - output->size()) return false;
        output->reserve(std::min(expectedSize, output->size() + audioSize));

        const std::size_t valuesPerChunk = std::size_t(channels) * 4096;
        std::vector<std::int32_t> samples(valuesPerChunk);
        std::int64_t remaining = sampleCount;
        const int qualifyMode = WavpackGetQualifyMode(context.get());
        while (remaining > 0) {
            const auto request = std::uint32_t(std::min<std::int64_t>(remaining, 4096));
            const std::uint32_t decoded = WavpackUnpackSamples(context.get(), samples.data(), request);
            if (!decoded) return false;
            const std::size_t values = std::size_t(decoded) * std::size_t(channels);
            for (std::size_t i = 0; i < values; ++i) {
                std::uint32_t value = std::uint32_t(samples[i]);
                if (bytesPerSample == 1 && !(qualifyMode & QMODE_SIGNED_BYTES))
                    value += 0x80;
                for (int byte = 0; byte < bytesPerSample; ++byte)
                    output->push_back(std::uint8_t(value >> (byte * 8)));
            }
            remaining -= decoded;
        }

        if (WavpackGetNumErrors(context.get())) return false;
        WavpackSeekTrailingWrapper(context.get());
        if (!appendWrapper(context.get(), output)) return false;
        return output->size() == expectedSize;
    } catch (...) {
        output->clear();
        return false;
    }
}

} // namespace compression
} // namespace peare
