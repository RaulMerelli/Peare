#include "LzmaDecoder.h"

#include "third_party/lzma/LzmaDec.h"
#include "third_party/lzma/Lzma2Dec.h"
#include "third_party/lzma/Ppmd7.h"

#include <cstdlib>
#include <limits>

namespace peare {
namespace compression {
namespace {

void* allocate(ISzAllocPtr, size_t size)
{
    return size ? std::malloc(size) : nullptr;
}

void release(ISzAllocPtr, void* address)
{
    std::free(address);
}

const ISzAlloc kAllocator = {allocate, release};
constexpr std::uint32_t kMaximumDictionarySize = 512u * 1024u * 1024u;
constexpr std::uint32_t kMaximumPpmdMemorySize = 512u * 1024u * 1024u;

struct PpmdMemoryInput {
    IByteIn interface;
    const Byte* data;
    std::size_t size;
    std::size_t position;
    bool exhausted;
};

Byte readPpmdByte(IByteInPtr interface)
{
    PpmdMemoryInput* input = reinterpret_cast<PpmdMemoryInput*>(
        const_cast<IByteIn*>(interface));
    if (input->position == input->size) {
        input->exhausted = true;
        return 0;
    }
    return input->data[input->position++];
}

}  // namespace

bool decodeLzmaRaw(const std::uint8_t* packed, std::size_t packedSize,
                   const std::uint8_t* properties, std::size_t propertiesSize,
                   std::size_t unpackedSize, std::vector<std::uint8_t>* output)
{
    if (!output || (!packed && packedSize) || (!properties && propertiesSize) ||
        unpackedSize > std::numeric_limits<SizeT>::max() ||
        packedSize > std::numeric_limits<SizeT>::max())
        return false;
    CLzmaProps decodedProperties = {};
    if (LzmaProps_Decode(&decodedProperties, properties,
                         static_cast<unsigned>(propertiesSize)) != SZ_OK ||
        decodedProperties.dicSize > kMaximumDictionarySize)
        return false;
    std::vector<std::uint8_t> result(unpackedSize);
    SizeT destinationSize = static_cast<SizeT>(result.size());
    SizeT sourceSize = static_cast<SizeT>(packedSize);
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    const SRes resultCode = LzmaDecode(result.data(), &destinationSize, packed, &sourceSize,
                                       properties, static_cast<unsigned>(propertiesSize),
                                       LZMA_FINISH_END, &status, &kAllocator);
    if (resultCode != SZ_OK || destinationSize != unpackedSize || sourceSize != packedSize)
        return false;
    output->swap(result);
    return true;
}

bool decodeLzma2Raw(const std::uint8_t* packed, std::size_t packedSize,
                    const std::uint8_t* properties, std::size_t propertiesSize,
                    std::size_t unpackedSize, std::vector<std::uint8_t>* output)
{
    if (!output || (!packed && packedSize) || !properties || propertiesSize != 1 ||
        properties[0] > 40 || unpackedSize > std::numeric_limits<SizeT>::max() ||
        packedSize > std::numeric_limits<SizeT>::max())
        return false;
    std::vector<std::uint8_t> result(unpackedSize);
    SizeT destinationSize = static_cast<SizeT>(result.size());
    SizeT sourceSize = static_cast<SizeT>(packedSize);
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    const SRes resultCode = Lzma2Decode(result.data(), &destinationSize, packed, &sourceSize,
                                        properties[0], LZMA_FINISH_END, &status, &kAllocator);
    if (resultCode != SZ_OK || destinationSize != unpackedSize || sourceSize != packedSize ||
        status != LZMA_STATUS_FINISHED_WITH_MARK)
        return false;
    output->swap(result);
    return true;
}

bool decodePpmd7Raw(const std::uint8_t* packed, std::size_t packedSize,
                    const std::uint8_t* properties, std::size_t propertiesSize,
                    std::size_t unpackedSize, std::vector<std::uint8_t>* output)
{
    if (!output || (!packed && packedSize) || !properties || propertiesSize != 5 ||
        unpackedSize > std::numeric_limits<SizeT>::max())
        return false;
    const unsigned order = properties[0];
    const std::uint32_t memorySize = std::uint32_t(properties[1]) |
        (std::uint32_t(properties[2]) << 8) |
        (std::uint32_t(properties[3]) << 16) |
        (std::uint32_t(properties[4]) << 24);
    if (order < PPMD7_MIN_ORDER || order > PPMD7_MAX_ORDER ||
        memorySize < PPMD7_MIN_MEM_SIZE || memorySize > kMaximumPpmdMemorySize)
        return false;

    std::vector<std::uint8_t> result(unpackedSize);
    CPpmd7 model;
    Ppmd7_Construct(&model);
    if (!Ppmd7_Alloc(&model, memorySize, &kAllocator)) return false;
    Ppmd7_Init(&model, order);
    PpmdMemoryInput input = {{readPpmdByte}, packed, packedSize, 0, false};
    model.rc.dec.Stream = &input.interface;
    bool valid = Ppmd7z_RangeDec_Init(&model.rc.dec) && !input.exhausted;
    for (std::size_t i = 0; valid && i < result.size(); ++i) {
        const int symbol = Ppmd7z_DecodeSymbol(&model);
        if (symbol < 0 || input.exhausted) valid = false;
        else result[i] = static_cast<std::uint8_t>(symbol);
    }
    valid = valid && Ppmd7z_RangeDec_IsFinishedOK(&model.rc.dec) &&
        !input.exhausted && input.position == packedSize;
    Ppmd7_Free(&model, &kAllocator);
    if (!valid) return false;
    output->swap(result);
    return true;
}

}  // namespace compression
}  // namespace peare
