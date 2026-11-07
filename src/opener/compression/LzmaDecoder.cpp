#include "LzmaDecoder.h"

#include "third_party/lzma/LzmaDec.h"
#include "third_party/lzma/Lzma2Dec.h"
#include "third_party/lzma/Ppmd7.h"
#include "third_party/lzma/Ppmd8.h"

#include <cstdlib>
#include <limits>

extern "C" {
BoolInt Ppmd7r_RangeDec_Init(CPpmd7_RangeDec* p);
int Ppmd7r_DecodeSymbol(CPpmd7* p);
}

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
                         static_cast<unsigned>(propertiesSize)) != SZ_OK)
        return false;
    std::vector<std::uint8_t> result;
    try {
        result.resize(unpackedSize);
    } catch (...) {
        return false;
    }
    Byte emptyDestination = 0;
    Byte* destination = result.empty() ? &emptyDestination : result.data();
    SizeT destinationSize = static_cast<SizeT>(result.size());
    SizeT sourceSize = static_cast<SizeT>(packedSize);
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    const SRes resultCode = LzmaDecode(destination, &destinationSize, packed, &sourceSize,
                                       properties, static_cast<unsigned>(propertiesSize),
                                       LZMA_FINISH_END, &status, &kAllocator);
    if (resultCode != SZ_OK || destinationSize != unpackedSize || sourceSize != packedSize)
        return false;
    output->swap(result);
    return true;
}

bool decodeLzmaRawWithEndMarker(const std::uint8_t* packed, std::size_t packedSize,
                                const std::uint8_t* properties, std::size_t propertiesSize,
                                std::size_t unpackedSize,
                                std::vector<std::uint8_t>* output)
{
    if (!output || (!packed && packedSize) || (!properties && propertiesSize) ||
        unpackedSize > std::numeric_limits<SizeT>::max() ||
        packedSize > std::numeric_limits<SizeT>::max())
        return false;
    CLzmaProps decodedProperties = {};
    if (LzmaProps_Decode(&decodedProperties, properties,
                         static_cast<unsigned>(propertiesSize)) != SZ_OK)
        return false;
    std::vector<std::uint8_t> result;
    try {
        result.resize(unpackedSize);
    } catch (...) {
        return false;
    }
    Byte emptyDestination = 0;
    Byte* destination = result.empty() ? &emptyDestination : result.data();
    SizeT destinationSize = static_cast<SizeT>(result.size());
    SizeT sourceSize = static_cast<SizeT>(packedSize);
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    const SRes resultCode = LzmaDecode(destination, &destinationSize, packed, &sourceSize,
                                       properties, static_cast<unsigned>(propertiesSize),
                                       LZMA_FINISH_END, &status, &kAllocator);
    if (resultCode != SZ_OK || destinationSize != unpackedSize ||
        sourceSize != packedSize || status != LZMA_STATUS_FINISHED_WITH_MARK)
        return false;
    output->swap(result);
    return true;
}

static bool decodeLzmaRawUntilEndMarkerImpl(
    const std::uint8_t* packed, std::size_t packedSize,
    const std::uint8_t* properties, std::size_t propertiesSize,
    bool requireAllInput, std::vector<std::uint8_t>* output)
{
    if (!output || (!packed && packedSize) || !properties ||
        propertiesSize != LZMA_PROPS_SIZE ||
        packedSize > std::numeric_limits<SizeT>::max()) return false;
    CLzmaProps decodedProperties = {};
    if (LzmaProps_Decode(&decodedProperties, properties,
                         static_cast<unsigned>(propertiesSize)) != SZ_OK) return false;
    CLzmaDec decoder;
    LzmaDec_Construct(&decoder);
    if (LzmaDec_Allocate(&decoder, properties, static_cast<unsigned>(propertiesSize),
                         &kAllocator) != SZ_OK) {
        LzmaDec_Free(&decoder, &kAllocator);
        return false;
    }
    LzmaDec_Init(&decoder);

    std::vector<std::uint8_t> decoded;
    std::size_t sourceOffset = 0;
    bool valid = false;
    try {
        std::vector<std::uint8_t> chunk(64 * 1024);
        while (true) {
            SizeT sourceSize = static_cast<SizeT>(packedSize - sourceOffset);
            SizeT destinationSize = static_cast<SizeT>(chunk.size());
            ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
            const Byte* source = packed ? packed + sourceOffset : nullptr;
            const SRes result = LzmaDec_DecodeToBuf(
                &decoder, chunk.data(), &destinationSize, source,
                &sourceSize, LZMA_FINISH_ANY, &status);
            sourceOffset += static_cast<std::size_t>(sourceSize);
            if (destinationSize > decoded.max_size() - decoded.size()) break;
            decoded.insert(decoded.end(), chunk.begin(), chunk.begin() + destinationSize);
            if (status == LZMA_STATUS_FINISHED_WITH_MARK) {
                valid = result == SZ_OK && (!requireAllInput || sourceOffset == packedSize);
                break;
            }
            if (result != SZ_OK || (sourceSize == 0 && destinationSize == 0)) break;
        }
    } catch (...) {
        valid = false;
    }
    LzmaDec_Free(&decoder, &kAllocator);
    if (!valid) return false;
    output->swap(decoded);
    return true;
}

bool decodeLzmaRawUntilEndMarker(const std::uint8_t* packed, std::size_t packedSize,
                                 const std::uint8_t* properties, std::size_t propertiesSize,
                                 std::vector<std::uint8_t>* output)
{
    return decodeLzmaRawUntilEndMarkerImpl(packed, packedSize, properties,
                                           propertiesSize, true, output);
}

bool decodeLzmaRawUntilEndMarkerWithTrailing(
    const std::uint8_t* packed, std::size_t packedSize,
    const std::uint8_t* properties, std::size_t propertiesSize,
    std::vector<std::uint8_t>* output)
{
    return decodeLzmaRawUntilEndMarkerImpl(packed, packedSize, properties,
                                           propertiesSize, false, output);
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

bool decodePpmdZip(const std::uint8_t* packed, std::size_t packedSize,
                   std::size_t unpackedSize, std::vector<std::uint8_t>* output)
{
    if (!output || !packed || packedSize < 7 ||
        unpackedSize > std::numeric_limits<SizeT>::max()) return false;
    const std::uint16_t parameters = std::uint16_t(packed[0]) |
        (std::uint16_t(packed[1]) << 8);
    const unsigned order = (parameters & 0x0Fu) + 1;
    const unsigned memoryMiB = ((parameters >> 4) & 0xFFu) + 1;
    const unsigned restoreMethod = parameters >> 12;
    if (order < PPMD8_MIN_ORDER || order > PPMD8_MAX_ORDER || restoreMethod > 2 ||
        memoryMiB > 256)
        return false;

    std::vector<std::uint8_t> result;
    try {
        result.resize(unpackedSize);
    } catch (...) {
        return false;
    }
    CPpmd8 model;
    Ppmd8_Construct(&model);
    const std::uint32_t memorySize = memoryMiB * 1024u * 1024u;
    if (!Ppmd8_Alloc(&model, memorySize, &kAllocator)) return false;
    PpmdMemoryInput input = {{readPpmdByte}, packed + 2, packedSize - 2, 0, false};
    model.Stream.In = &input.interface;
    bool valid = Ppmd8_Init_RangeDec(&model) && !input.exhausted;
    if (valid) Ppmd8_Init(&model, order, restoreMethod);
    for (std::size_t i = 0; valid && i < result.size(); ++i) {
        const int symbol = Ppmd8_DecodeSymbol(&model);
        if (symbol < 0 || input.exhausted) valid = false;
        else result[i] = static_cast<std::uint8_t>(symbol);
    }
    if (valid) valid = Ppmd8_DecodeSymbol(&model) == -1 && !input.exhausted &&
        Ppmd8_RangeDec_IsFinishedOK(&model) && input.position == input.size;
    Ppmd8_Free(&model, &kAllocator);
    if (!valid) return false;
    output->swap(result);
    return true;
}

}  // namespace compression
}  // namespace peare
