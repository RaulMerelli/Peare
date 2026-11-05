#include "Rar4LzDecoder.h"
#include "../compression/peare/Rar4PpmdDecoder.h"
#include "Rar4Vm.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <limits>

namespace peare {
namespace rar4 {

namespace {

class BitReader final {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool read(unsigned count, std::uint32_t* value)
    {
        if (!value || count > 32 || size_ > std::numeric_limits<std::size_t>::max() / 8 ||
            bitPos_ > size_ * 8 || count > size_ * 8 - bitPos_)
            return false;
        std::uint32_t result = 0;
        for (unsigned i = 0; i < count; ++i) {
            const std::size_t byte = bitPos_ / 8;
            const unsigned shift = 7U - unsigned(bitPos_ % 8);
            result = (result << 1) | ((data_[byte] >> shift) & 1U);
            ++bitPos_;
        }
        *value = result;
        return true;
    }

    bool alignByte()
    {
        if (bitPos_ > std::numeric_limits<std::size_t>::max() - 7) return false;
        const std::size_t aligned = (bitPos_ + 7) & ~std::size_t(7);
        if (size_ > std::numeric_limits<std::size_t>::max() / 8 || aligned > size_ * 8)
            return false;
        bitPos_ = aligned;
        return true;
    }

    std::size_t remainingBits() const
    {
        if (size_ > std::numeric_limits<std::size_t>::max() / 8)
            return std::numeric_limits<std::size_t>::max();
        return bitPos_ <= size_ * 8 ? size_ * 8 - bitPos_ : 0;
    }

    bool bytePosition(std::size_t* position) const
    {
        if (!position || bitPos_ % 8 != 0) return false;
        *position = bitPos_ / 8;
        return true;
    }

    bool seekBytePosition(std::size_t position)
    {
        if (position > size_ || position > std::numeric_limits<std::size_t>::max() / 8)
            return false;
        bitPos_ = position * 8;
        return true;
    }

    bool readData(std::uint32_t* value)
    {
        std::uint32_t form = 0;
        if (!read(2, &form)) return false;
        if (form == 0) return read(4, value);
        if (form == 1) {
            std::uint32_t marker = 0;
            if (!read(4, &marker)) return false;
            if (marker == 0) {
                std::uint32_t byte = 0;
                if (!read(8, &byte)) return false;
                *value = (byte & 0x80U) ? byte | 0xffffff00U : byte;
            } else {
                std::uint32_t low = 0;
                if (!read(4, &low)) return false;
                *value = (marker << 4) | low;
            }
            return true;
        }
        if (form == 2) return read(16, value);
        std::uint32_t high = 0;
        std::uint32_t low = 0;
        if (!read(16, &high) || !read(16, &low)) return false;
        *value = (high << 16) | low;
        return true;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t bitPos_ = 0;
};

struct PpmdMemoryInput {
    const Byte* data = nullptr;
    std::size_t size = 0;
    std::size_t position = 0;
    bool exhausted = false;

    PpmdMemoryInput(const Byte* bytes, std::size_t byteCount)
        : data(bytes), size(byteCount) {}
};

struct FilterRecord {
    std::uint8_t firstByte = 0;
    std::size_t filterIndex = 0;
    std::uint32_t outputStart = 0;
    std::uint32_t blockLength = 0;
    std::array<std::uint32_t, 7> registers{};
    std::vector<std::uint8_t> code;
};

bool parseFilterRecord(std::uint8_t firstByte, const std::vector<std::uint8_t>& record,
                      std::size_t outputPosition, std::vector<FilterRecord>* programs,
                      std::size_t* lastFilter, FilterRecord* filter)
{
    filter->firstByte = firstByte;
    BitReader codeBits(record.data(), record.size());
    std::uint32_t filterIndex = 0;
    if (firstByte & 0x80U) {
        if (!codeBits.readData(&filterIndex) || filterIndex > 0x1000U) return false;
        if (filterIndex == 0) programs->clear();
        else --filterIndex;
    } else {
        filterIndex = static_cast<std::uint32_t>(*lastFilter);
    }
    if (filterIndex > programs->size()) return false;
    const bool newProgram = filterIndex == programs->size();
    filter->filterIndex = filterIndex;
    *lastFilter = filterIndex;
    std::uint32_t blockStart = 0;
    if (!codeBits.readData(&blockStart)) return false;
    if ((filter->firstByte & 0x40U) && blockStart > 0xffffffffU - 258U) return false;
    if (filter->firstByte & 0x40U) blockStart += 258U;
    if (blockStart > 0xffffffffU - outputPosition) return false;
    filter->outputStart = static_cast<std::uint32_t>(outputPosition + blockStart);
    if (filter->firstByte & 0x20U) {
        if (!codeBits.readData(&filter->blockLength)) return false;
    } else {
        filter->blockLength = newProgram ? 0 : (*programs)[filterIndex].blockLength;
    }
    filter->registers[4] = filter->blockLength;
    if (filter->firstByte & 0x10U) {
        std::uint32_t mask = 0;
        if (!codeBits.read(7, &mask)) return false;
        for (unsigned i = 0; i < 7; ++i)
            if ((mask & (1U << i)) && !codeBits.readData(&filter->registers[i]))
                return false;
    }
    if (newProgram) {
        std::uint32_t vmCodeSize = 0;
        if (!codeBits.readData(&vmCodeSize) || vmCodeSize == 0 || vmCodeSize >= 0x10000U)
            return false;
        filter->code.resize(vmCodeSize);
        for (std::uint32_t i = 0; i < vmCodeSize; ++i) {
            std::uint32_t byte = 0;
            if (!codeBits.read(8, &byte)) return false;
            filter->code[i] = static_cast<std::uint8_t>(byte);
        }
        programs->push_back(*filter);
    } else {
        filter->code = (*programs)[filterIndex].code;
    }
    return true;
}

bool readPpmdFilter(const std::function<int()>& readSymbol,
                    std::size_t outputPosition, std::vector<FilterRecord>* programs,
                    std::size_t* lastFilter,
                    FilterRecord* filter)
{
    const int first = readSymbol();
    if (first < 0) return false;
    const std::uint8_t firstByte = static_cast<std::uint8_t>(first);
    std::uint32_t recordLength = (firstByte & 7U) + 1U;
    if (recordLength == 7) {
        const int extra = readSymbol();
        if (extra < 0) return false;
        recordLength = static_cast<std::uint32_t>(extra) + 7U;
    } else if (recordLength == 8) {
        const int high = readSymbol();
        const int low = readSymbol();
        if (high < 0 || low < 0) return false;
        recordLength = static_cast<std::uint32_t>(high * 256 + low);
    }
    if (recordLength == 0 || recordLength > 0xffffU) return false;
    std::vector<std::uint8_t> record(recordLength);
    for (std::uint32_t i = 0; i < recordLength; ++i) {
        const int byte = readSymbol();
        if (byte < 0) return false;
        record[i] = static_cast<std::uint8_t>(byte);
    }
    return parseFilterRecord(firstByte, record, outputPosition, programs, lastFilter, filter);
}

bool readLzFilter(BitReader* bits, std::size_t outputPosition,
                  std::vector<FilterRecord>* programs, std::size_t* lastFilter,
                  FilterRecord* filter)
{
    std::uint32_t firstValue = 0;
    if (!bits->read(8, &firstValue)) return false;
    const std::uint8_t firstByte = static_cast<std::uint8_t>(firstValue);
    std::uint32_t recordLength = (firstByte & 7U) + 1U;
    if (recordLength == 7) {
        std::uint32_t extra = 0;
        if (!bits->read(8, &extra)) return false;
        recordLength = extra + 7U;
    } else if (recordLength == 8) {
        std::uint32_t extended = 0;
        if (!bits->read(16, &extended)) return false;
        recordLength = extended;
    }
    if (recordLength == 0 || recordLength > 0xffffU) return false;
    std::vector<std::uint8_t> record(recordLength);
    for (std::uint32_t i = 0; i < recordLength; ++i) {
        std::uint32_t byte = 0;
        if (!bits->read(8, &byte)) return false;
        record[i] = static_cast<std::uint8_t>(byte);
    }
    return parseFilterRecord(firstByte, record, outputPosition, programs, lastFilter, filter);
}

int readPpmdByte(void* context, std::uint8_t* value)
{
    auto* input = static_cast<PpmdMemoryInput*>(context);
    if (!input || !value || input->position >= input->size) {
        if (input) input->exhausted = true;
        return 0;
    }
    *value = input->data[input->position++];
    return 1;
}

void* allocPpmdMemory(ISzAllocPtr, size_t size)
{
    return size ? std::malloc(size) : nullptr;
}

void freePpmdMemory(ISzAllocPtr, void* address)
{
    std::free(address);
}

const ISzAlloc kPpmdAllocator = {allocPpmdMemory, freePpmdMemory};

class Huffman final {
public:
    bool build(const std::uint8_t* lengths, std::size_t count)
    {
        if (!lengths || count == 0) return false;
        symbols_.clear();
        firstCode_.fill(0);
        firstSymbol_.fill(0);
        counts_.fill(0);
        maxLength_ = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (lengths[i] > 15) return false;
            if (lengths[i]) {
                ++counts_[lengths[i]];
                if (lengths[i] > maxLength_) maxLength_ = lengths[i];
            }
        }
        if (maxLength_ == 0) return true;
        std::uint32_t code = 0;
        std::size_t symbolIndex = 0;
        for (unsigned length = 1; length <= maxLength_; ++length) {
            code = (code + counts_[length - 1]) << 1;
            const std::uint32_t limit = std::uint32_t(1) << length;
            if (code > limit || counts_[length] > limit - code) return false;
            firstCode_[length] = code;
            firstSymbol_[length] = symbolIndex;
            symbolIndex += counts_[length];
        }
        symbols_.reserve(symbolIndex);
        for (unsigned length = 1; length <= maxLength_; ++length) {
            for (std::size_t symbol = 0; symbol < count; ++symbol)
                if (lengths[symbol] == length)
                    symbols_.push_back(static_cast<std::uint16_t>(symbol));
        }
        return symbols_.size() == symbolIndex;
    }

    bool decode(BitReader* bits, std::uint16_t* symbol) const
    {
        if (!bits || !symbol || maxLength_ == 0) return false;
        std::uint32_t code = 0;
        for (unsigned length = 1; length <= maxLength_; ++length) {
            std::uint32_t bit = 0;
            if (!bits->read(1, &bit)) return false;
            code = (code << 1) | bit;
            if (code >= firstCode_[length] &&
                code - firstCode_[length] < counts_[length]) {
                const std::size_t index = firstSymbol_[length] +
                    std::size_t(code - firstCode_[length]);
                if (index >= symbols_.size()) return false;
                *symbol = symbols_[index];
                return true;
            }
        }
        return false;
    }

private:
    std::array<std::uint16_t, 16> counts_{};
    std::array<std::uint32_t, 16> firstCode_{};
    std::array<std::size_t, 16> firstSymbol_{};
    std::vector<std::uint16_t> symbols_;
    unsigned maxLength_ = 0;
};

struct Tables {
    std::array<std::uint8_t, 404> lengths{};
    Huffman precode;
    Huffman main;
    Huffman distance;
    Huffman lowDistance;
    Huffman length;

    bool build(BitReader* bits, bool retain)
    {
        if (!bits) return false;
        if (!retain) lengths.fill(0);
        std::array<std::uint8_t, 20> preLengths{};
        std::size_t preCount = 0;
        while (preCount < preLengths.size()) {
            std::uint32_t value = 0;
            if (!bits->read(4, &value)) return false;
            if (value == 15) {
                std::uint32_t zeroCount = 0;
                if (!bits->read(4, &zeroCount)) return false;
                if (zeroCount != 0) {
                    const std::size_t run = std::size_t(zeroCount) + 2;
                    if (run > preLengths.size() - preCount) return false;
                    for (std::size_t i = 0; i < run; ++i)
                        preLengths[preCount++] = 0;
                    continue;
                }
            }
            preLengths[preCount++] = static_cast<std::uint8_t>(value);
        }
        if (!precode.build(preLengths.data(), preLengths.size())) return false;

        std::size_t index = 0;
        std::uint8_t previous = 0;
        while (index < lengths.size()) {
            std::uint16_t symbol = 0;
            if (!precode.decode(bits, &symbol)) return false;
            if (symbol <= 15) {
                previous = std::uint8_t((lengths[index] + symbol) & 15U);
                lengths[index++] = previous;
                continue;
            }
            std::uint32_t extra = 0;
            std::size_t run = 0;
            std::uint8_t repeated = previous;
            if (symbol == 16 || symbol == 17) {
                if (index == 0) return false;
                if (symbol == 16) {
                    if (!bits->read(3, &extra)) return false;
                    run = std::size_t(extra) + 3;
                } else {
                    if (!bits->read(7, &extra)) return false;
                    run = std::size_t(extra) + 11;
                }
            } else if (symbol == 18 || symbol == 19) {
                repeated = 0;
                previous = 0;
                if (symbol == 18) {
                    if (!bits->read(3, &extra)) return false;
                    run = std::size_t(extra) + 3;
                } else {
                    if (!bits->read(7, &extra)) return false;
                    run = std::size_t(extra) + 11;
                }
            } else {
                return false;
            }
            if (run > lengths.size() - index) return false;
            for (std::size_t i = 0; i < run; ++i)
                lengths[index++] = repeated;
            previous = repeated;
        }

        const bool mainOk = main.build(lengths.data(), 299);
        const bool distanceOk = distance.build(lengths.data() + 299, 60);
        const bool lowOk = lowDistance.build(lengths.data() + 359, 17);
        const bool lengthOk = length.build(lengths.data() + 376, 28);
        return mainOk && distanceOk && lowOk && lengthOk;
    }
};

const std::array<std::uint32_t, 28> kLengthBase = {{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20,
    24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224
}};

const std::array<std::uint8_t, 28> kLengthExtra = {{
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
    2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5
}};

const std::array<std::uint32_t, 8> kShortDistanceBase = {{0, 4, 8, 16, 32, 64, 128, 192}};
const std::array<std::uint8_t, 8> kShortDistanceExtra = {{2, 2, 3, 4, 5, 6, 6, 6}};

const std::array<std::uint32_t, 60> kDistanceBase = {{
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192,
    256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192,
    12288, 16384, 24576, 32768, 49152, 65536, 98304, 131072, 196608,
    262144, 327680, 393216, 458752, 524288, 589824, 655360, 720896,
    786432, 851968, 917504, 983040, 1048576, 1310720, 1572864,
    1835008, 2097152, 2359296, 2621440, 2883584, 3145728, 3407872,
    3670016, 3932160
}};

bool appendByte(std::vector<std::uint8_t>* output, std::size_t limit,
                std::uint8_t value, LzState* state)
{
    if (!output || !state || output->size() >= limit) return false;
    output->push_back(value);
    state->pushHistory(value);
    return true;
}

bool appendMatch(std::vector<std::uint8_t>* output, std::size_t limit,
                 std::uint32_t distance, std::uint32_t length,
                 std::size_t historyWindow, LzState* state)
{
    if (!output || !state || output->size() > limit || distance == 0 || distance > historyWindow ||
        length > limit - output->size())
        return false;
    for (std::uint32_t i = 0; i < length; ++i) {
        std::uint8_t value = 0;
        if (!state->historyAtDistance(distance, historyWindow, &value)) return false;
        output->push_back(value);
        state->pushHistory(value);
    }
    return true;
}

bool decodeLength(BitReader* bits, const Huffman& table, std::uint32_t bias,
                 std::uint32_t* result)
{
    std::uint16_t symbol = 0;
    if (!table.decode(bits, &symbol) || symbol >= kLengthBase.size()) return false;
    std::uint32_t extra = 0;
    if (!bits->read(kLengthExtra[symbol], &extra)) return false;
    const std::uint64_t value = std::uint64_t(kLengthBase[symbol]) + extra + bias;
    if (value > std::numeric_limits<std::uint32_t>::max()) return false;
    *result = std::uint32_t(value);
    return true;
}

bool decodeTables(BitReader* bits, Tables* tables, bool retain, LzState* state)
{
    if (!bits || !tables || !state || !tables->build(bits, retain))
        return false;
    state->priorLengths = tables->lengths;
    state->hasPriorLengths = true;
    return true;
}

bool beginLzBlock(BitReader* bits, Tables* tables, LzState* state)
{
    std::uint32_t retain = 0;
    if (!bits || !tables || !state || !bits->read(1, &retain)) return false;
    if (retain) {
        if (!state->hasPriorLengths) return false;
        tables->lengths = state->priorLengths;
    }
    return decodeTables(bits, tables, retain != 0, state);
}

bool decodeDistance(BitReader* bits, const Tables& tables,
                    std::uint32_t* previousLow, bool* hasPreviousLow,
                    unsigned* repeatedLow,
                    std::uint32_t* result)
{
    std::uint16_t symbol = 0;
    if (!tables.distance.decode(bits, &symbol) || symbol >= kDistanceBase.size()) return false;
    std::uint32_t extraBits = 0;
    if (symbol < 10) {
        const std::uint32_t delta = kDistanceBase[symbol + 1] - kDistanceBase[symbol];
        while ((std::uint32_t(1) << extraBits) < delta) ++extraBits;
        std::uint32_t extra = 0;
        if (!bits->read(extraBits, &extra)) return false;
        const std::uint64_t distance = std::uint64_t(kDistanceBase[symbol]) + extra + 1;
        if (distance > std::numeric_limits<std::uint32_t>::max()) return false;
        *result = std::uint32_t(distance);
        return true;
    }

    std::uint32_t delta = symbol + 1 < kDistanceBase.size()
        ? kDistanceBase[symbol + 1] - kDistanceBase[symbol]
        : kDistanceBase[symbol] - kDistanceBase[symbol - 1];
    while ((std::uint32_t(1) << extraBits) < delta) ++extraBits;
    if (extraBits < 4) return false;
    std::uint32_t high = 0;
    if (!bits->read(extraBits - 4, &high)) return false;

    std::uint32_t low = 0;
    if (*repeatedLow) {
        --*repeatedLow;
        low = *previousLow;
    } else {
        std::uint16_t lowSymbol = 0;
        if (!tables.lowDistance.decode(bits, &lowSymbol)) return false;
        if (lowSymbol == 16) {
            if (!*hasPreviousLow) return false;
            *repeatedLow = 14;
            low = *previousLow;
        } else if (lowSymbol < 16) {
            low = lowSymbol;
            *previousLow = low;
            *hasPreviousLow = true;
        } else {
            return false;
        }
    }
    const std::uint64_t value = std::uint64_t(kDistanceBase[symbol]) +
        (std::uint64_t(high) << 4) + low + 1;
    if (value > std::numeric_limits<std::uint32_t>::max()) return false;
    *result = std::uint32_t(value);
    return true;
}

void pushRecent(std::array<std::uint32_t, 4>* recent, std::uint32_t distance)
{
    for (std::size_t i = recent->size() - 1; i > 0; --i)
        (*recent)[i] = (*recent)[i - 1];
    (*recent)[0] = distance;
}

enum class BlockResult { Done, Next, Error };

BlockResult decodeLzBlock(BitReader* bits, std::size_t outputSize,
                          std::size_t historyWindow, LzState* state,
                          std::vector<std::uint8_t>* output,
                          std::vector<FilterRecord>* filters,
                          std::vector<FilterRecord>* filterPrograms,
                          std::size_t* lastFilter)
{
    Tables tables;
    if (!beginLzBlock(bits, &tables, state)) return BlockResult::Error;
    const bool emptyOutput = outputSize == 0;

    while (output->size() < outputSize || emptyOutput) {
        std::uint16_t symbol = 0;
        if (!tables.main.decode(bits, &symbol)) return BlockResult::Error;
        if (symbol < 256) {
            if (!appendByte(output, outputSize, std::uint8_t(symbol), state))
                return BlockResult::Error;
            continue;
        }
        if (symbol == 256) {
            std::uint32_t first = 0;
            if (!bits->read(1, &first)) return BlockResult::Error;
            if (first != 0) {
                state->repeatedLowDistance = 0;
                return BlockResult::Next;
            }
            std::uint32_t second = 0;
            if (!bits->read(1, &second)) return BlockResult::Error;
            (void)second;
            return output->size() == outputSize ? BlockResult::Done : BlockResult::Error;
        }
        if (symbol == 257) {
            FilterRecord filter;
            if (!readLzFilter(bits, output->size(), filterPrograms, lastFilter, &filter))
                return BlockResult::Error;
            if (filter.filterIndex < filterPrograms->size())
                (*filterPrograms)[filter.filterIndex].blockLength = filter.blockLength;
            filters->push_back(std::move(filter));
            continue;
        }

        std::uint32_t distance = 0;
        std::uint32_t length = 0;
        if (symbol == 258) {
            if (state->lastDistance == 0 || state->lastLength == 0)
                return BlockResult::Error;
            distance = state->lastDistance;
            length = state->lastLength;
        } else if (symbol >= 259 && symbol <= 262) {
            const std::size_t selected = symbol - 259;
            if (state->recentDistances[selected] == 0 ||
                !decodeLength(bits, tables.length, 2, &length))
                return BlockResult::Error;
            distance = state->recentDistances[selected];
            for (std::size_t i = selected; i > 0; --i)
                state->recentDistances[i] = state->recentDistances[i - 1];
            state->recentDistances[0] = distance;
        } else if (symbol >= 263 && symbol <= 270) {
            const std::size_t index = symbol - 263;
            std::uint32_t extra = 0;
            if (!bits->read(kShortDistanceExtra[index], &extra)) return BlockResult::Error;
            distance = kShortDistanceBase[index] + extra + 1;
            length = 2;
            pushRecent(&state->recentDistances, distance);
        } else if (symbol >= 271 && symbol <= 298) {
            const std::size_t lengthIndex = symbol - 271;
            std::uint32_t lengthExtra = 0;
            if (!bits->read(kLengthExtra[lengthIndex], &lengthExtra) ||
                !decodeDistance(bits, tables, &state->previousLowDistance,
                                &state->hasPreviousLowDistance,
                                &state->repeatedLowDistance, &distance))
                return BlockResult::Error;
            length = kLengthBase[lengthIndex] + lengthExtra + 3;
            if (distance >= 0x2000) ++length;
            if (distance >= 0x40000) ++length;
            pushRecent(&state->recentDistances, distance);
        } else {
            return BlockResult::Error;
        }
        if (output->size() < outputSize)
            length = std::min<std::uint32_t>(
                length, static_cast<std::uint32_t>(outputSize - output->size()));
        if (!appendMatch(output, outputSize, distance, length, historyWindow, state))
            return BlockResult::Error;
        state->lastDistance = distance;
        state->lastLength = length;
    }
    return BlockResult::Done;
}

BlockResult decodePpmdBlock(BitReader* bits, const std::uint8_t* packed,
                            std::size_t packedSize, std::size_t outputSize,
                            std::size_t historyWindow, LzState* state,
                            std::vector<std::uint8_t>* output,
                            std::vector<FilterRecord>* filters,
                            std::vector<FilterRecord>* filterPrograms,
                            std::size_t* lastFilter)
{
    std::uint32_t flags = 0;
    if (!bits->read(7, &flags)) return BlockResult::Error;

    std::uint32_t memorySize = state->ppmdMemorySize;
    unsigned order = state->ppmdModel.MaxOrder;
    if (flags & 0x20) {
        std::uint32_t memoryByte = 0;
        if (!bits->read(8, &memoryByte)) return BlockResult::Error;
        memorySize = (memoryByte + 1) * 1024U * 1024U;
        const unsigned encodedOrder = unsigned(flags & 0x1f) + 1;
        order = encodedOrder > 16 ? 16 + (encodedOrder - 16) * 3 : encodedOrder;
        if (order < PPMD7_MIN_ORDER || order > PPMD7_MAX_ORDER)
            return BlockResult::Error;
    }

    std::uint8_t escapeValue = 2;
    const bool hasCustomEscape = (flags & 0x40) != 0;
    if (hasCustomEscape) {
        std::uint32_t escape = 0;
        if (!bits->read(8, &escape)) return BlockResult::Error;
        escapeValue = static_cast<std::uint8_t>(escape);
    }
    if (flags & 0x20) {
        state->resetPpmd();
        state->ppmdEscape = escapeValue;
        if (hasCustomEscape)
            state->ppmdModel.InitEsc = state->ppmdEscape;
        if (!Ppmd7_Alloc(&state->ppmdModel, memorySize, &kPpmdAllocator))
            return BlockResult::Error;
        state->ppmdMemorySize = memorySize;
        Ppmd7_Init(&state->ppmdModel, order);
        state->ppmdValid = true;
    } else if (!state->ppmdValid) {
        return BlockResult::Error;
    } else {
        state->ppmdEscape = escapeValue;
    }
    std::size_t startByte = 0;
    if (!bits->bytePosition(&startByte) || startByte > packedSize)
        return BlockResult::Error;
    const auto* packedBytes = reinterpret_cast<const Byte*>(packed);
    PpmdMemoryInput input(packedBytes ? packedBytes + startByte : nullptr,
                          packedSize - startByte);
    PeareRar4PpmdDecoder decoder{};
    if (!peare_rar4_ppmd_init(&decoder, readPpmdByte, &input) || input.exhausted)
        return BlockResult::Error;
    const auto readSymbol = [&]() -> int {
        const int symbol = peare_rar4_ppmd_decode_symbol(&decoder, &state->ppmdModel);
        return input.exhausted ? PPMD7_SYM_ERROR : symbol;
    };
    const auto finish = [&](BlockResult result) -> BlockResult {
        if (!bits->seekBytePosition(startByte + input.position))
            return BlockResult::Error;
        return result;
    };

    for (;;) {
        const int symbol = readSymbol();
        if (symbol < 0) return finish(BlockResult::Error);
        if (symbol != state->ppmdEscape) {
            if (!appendByte(output, outputSize, static_cast<std::uint8_t>(symbol), state))
                return finish(BlockResult::Error);
            continue;
        }

        const int command = readSymbol();
        if (command < 0) return finish(BlockResult::Error);
        if (command == 0) return finish(BlockResult::Next);
        if (command == 2) {
            return finish(output->size() == outputSize
                ? BlockResult::Done : BlockResult::Error);
        }
        if (command == 3) {
            FilterRecord filter;
            if (!readPpmdFilter(readSymbol, output->size(), filterPrograms,
                                lastFilter, &filter))
                return finish(BlockResult::Error);
            if (filter.filterIndex < filterPrograms->size())
                (*filterPrograms)[filter.filterIndex].blockLength = filter.blockLength;
            filters->push_back(std::move(filter));
            continue;
        }
        if (command == 4) {
            const int b0 = readSymbol();
            const int b1 = readSymbol();
            const int b2 = readSymbol();
            const int lengthSymbol = readSymbol();
            if (b0 < 0 || b1 < 0 || b2 < 0 || lengthSymbol < 0)
                return finish(BlockResult::Error);
            const std::uint32_t distance =
                (std::uint32_t(b0) << 16) | (std::uint32_t(b1) << 8) |
                std::uint32_t(b2);
            if (!appendMatch(output, outputSize, distance + 2,
                             std::uint32_t(lengthSymbol) + 32,
                             historyWindow, state))
                return finish(BlockResult::Error);
            continue;
        }
        if (command == 5) {
            const int lengthSymbol = readSymbol();
            if (lengthSymbol < 0 ||
                !appendMatch(output, outputSize, 1,
                             std::uint32_t(lengthSymbol) + 4,
                             historyWindow, state))
                return finish(BlockResult::Error);
            continue;
        }
        if (!appendByte(output, outputSize, state->ppmdEscape, state))
            return finish(BlockResult::Error);
    }
}

bool decodeStream(BitReader* bits, const std::uint8_t* packed,
                  std::size_t packedSize, std::size_t outputSize,
                  std::size_t historyWindow, LzState* state,
                  std::vector<std::uint8_t>* output,
                  std::vector<FilterRecord>* filters,
                  std::vector<FilterRecord>* filterPrograms,
                  std::size_t* lastFilter)
{
    for (std::size_t blockCount = 0; blockCount <= packedSize; ++blockCount) {
        if (!bits->alignByte()) return false;
        std::uint32_t mode = 0;
        if (!bits->read(1, &mode)) return false;
        const BlockResult result = mode == 0
            ? decodeLzBlock(bits, outputSize, historyWindow, state, output, filters,
                            filterPrograms, lastFilter)
            : decodePpmdBlock(bits, packed, packedSize, outputSize,
                              historyWindow, state, output, filters,
                              filterPrograms, lastFilter);
        if (result == BlockResult::Done) return output->size() == outputSize;
        if (result == BlockResult::Error) return false;
    }
    return false;
}

} // namespace

LzState::LzState() : history_(4 * 1024 * 1024, 0)
{
    Ppmd7_Construct(&ppmdModel);
}

LzState::~LzState()
{
    resetPpmd();
}

void LzState::resetPpmd()
{
    if (Ppmd7_WasAllocated(&ppmdModel))
        Ppmd7_Free(&ppmdModel, &kPpmdAllocator);
    Ppmd7_Construct(&ppmdModel);
    ppmdModel.InitEsc = 0;
    ppmdValid = false;
    ppmdMemorySize = 0;
    ppmdEscape = 2;
}

void LzState::reset()
{
    priorLengths.fill(0);
    hasPriorLengths = false;
    recentDistances.fill(0);
    previousLowDistance = 0;
    hasPreviousLowDistance = false;
    repeatedLowDistance = 0;
    lastDistance = 0;
    lastLength = 0;
    resetPpmd();
    historyWrite_ = 0;
    historyCount_ = 0;
}

void LzState::pushHistory(std::uint8_t value)
{
    if (history_.empty()) return;
    history_[historyWrite_] = value;
    historyWrite_ = (historyWrite_ + 1) % history_.size();
    if (historyCount_ < history_.size()) ++historyCount_;
}

bool LzState::historyAtDistance(std::uint32_t distance, std::size_t window,
                                std::uint8_t* value) const
{
    if (!value || distance == 0 || distance > window || distance > historyCount_ ||
        history_.empty())
        return false;
    const std::size_t index = (historyWrite_ + history_.size() - distance) % history_.size();
    *value = history_[index];
    return true;
}

bool decodeLz(const std::uint8_t* input, std::size_t inputSize,
              std::size_t outputSize, bool solid,
              std::size_t historyWindow, LzState* state,
              std::vector<std::uint8_t>* output)
{
    if ((!input && inputSize != 0) || !output || !state || historyWindow == 0)
        return false;
    if (!solid) state->reset();
    output->clear();
    if (outputSize > output->max_size()) return false;
    output->reserve(outputSize);
    BitReader bits(input, inputSize);
    std::vector<FilterRecord> filters;
    std::vector<FilterRecord> filterPrograms;
    std::size_t lastFilter = 0;
    const bool valid = decodeStream(&bits, input, inputSize, outputSize,
                                    historyWindow, state, output, &filters,
                                    &filterPrograms, &lastFilter) &&
        output->size() == outputSize;
    if (valid) {
        for (std::size_t filterIndex = 0; filterIndex < filters.size(); ++filterIndex) {
            const FilterRecord& filter = filters[filterIndex];
            if (filter.outputStart > output->size() ||
                filter.blockLength > output->size() - filter.outputStart ||
                filter.blockLength > 0x40000U)
                return false;
            std::vector<std::uint8_t> memory(0x40004U, 0);
            std::copy(output->begin() + filter.outputStart,
                      output->begin() + filter.outputStart + filter.blockLength,
                      memory.begin());
            VmState vmState;
            for (unsigned i = 0; i < filter.registers.size(); ++i)
                vmState.registers[i] = filter.registers[i];
            vmState.registers[3] = 0x3c000U;
            vmState.registers[4] = filter.blockLength;
            vmState.registers[6] = filter.outputStart;
            const VmResult vmResult = executeVm(filter.code, &memory, &vmState);
            if (!vmResult.success) return false;
            if (vmResult.outputPosition > memory.size() ||
                vmResult.outputSize > memory.size() - vmResult.outputPosition)
                return false;
            const std::vector<std::uint8_t> filtered(
                memory.begin() + vmResult.outputPosition,
                memory.begin() + vmResult.outputPosition + vmResult.outputSize);
            const std::size_t replaceEnd = filter.outputStart + filter.blockLength;
            output->erase(output->begin() + filter.outputStart,
                          output->begin() + replaceEnd);
            output->insert(output->begin() + filter.outputStart,
                           filtered.begin(), filtered.end());
            if (filtered.size() != filter.blockLength) {
                const std::int64_t delta = static_cast<std::int64_t>(filtered.size()) -
                    static_cast<std::int64_t>(filter.blockLength);
                for (std::size_t next = filterIndex + 1; next < filters.size(); ++next)
                    if (filters[next].outputStart >= replaceEnd)
                        filters[next].outputStart = static_cast<std::uint32_t>(
                            static_cast<std::int64_t>(filters[next].outputStart) + delta);
            }
        }
    }
    if (!valid) {
        output->clear();
        state->resetPpmd();
    }
    return valid;
}

} // namespace rar4
} // namespace peare
