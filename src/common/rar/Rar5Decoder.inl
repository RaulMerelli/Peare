#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

class Rar5BitReader final {
public:
    Rar5BitReader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

    bool read(unsigned count, std::uint32_t* value)
    {
        if (!value || count > 32) return false;
        std::uint64_t result = 0;
        if (!read64(count, &result)) return false;
        *value = std::uint32_t(result);
        return true;
    }

    bool read64(unsigned count, std::uint64_t* value)
    {
        if (!value || count > 64 || size_ > std::numeric_limits<std::size_t>::max() / 8 ||
            bit_ > size_ * 8 || count > size_ * 8 - bit_) return false;
        std::uint64_t result = 0;
        for (unsigned i = 0; i < count; ++i) {
            result = (result << 1) |
                ((data_[bit_ / 8] >> (7U - unsigned(bit_ & 7U))) & 1U);
            ++bit_;
        }
        *value = result;
        return true;
    }

    std::size_t position() const { return bit_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t bit_ = 0;
};

class Rar5Huffman final {
public:
    bool build(const std::vector<std::uint8_t>& lengths)
    {
        std::array<std::uint32_t, 16> counts{};
        for (std::uint8_t length : lengths) {
            if (length > 15) return false;
            if (length) ++counts[length];
        }
        std::uint32_t code = 0;
        std::uint32_t offset = 0;
        for (unsigned length = 1; length <= 15; ++length) {
            code = (code + counts[length - 1]) << 1;
            const std::uint32_t limit = 1U << length;
            if (code > limit || counts[length] > limit - code) return false;
            firstCode_[length] = code;
            firstSymbol_[length] = offset;
            count_[length] = counts[length];
            offset += counts[length];
        }
        if (!offset) {
            symbols_.clear();
            return true;
        }
        symbols_.clear();
        symbols_.reserve(offset);
        for (unsigned length = 1; length <= 15; ++length)
            for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol)
                if (lengths[symbol] == length) symbols_.push_back(std::uint32_t(symbol));
        return symbols_.size() == offset;
    }

    bool decode(Rar5BitReader* bits, std::uint32_t* symbol) const
    {
        if (!bits || !symbol) return false;
        std::uint32_t code = 0;
        for (unsigned length = 1; length <= 15; ++length) {
            std::uint32_t bit = 0;
            if (!bits->read(1, &bit)) return false;
            code = (code << 1) | bit;
            if (code >= firstCode_[length] &&
                code - firstCode_[length] < count_[length]) {
                const std::uint32_t index = firstSymbol_[length] + code - firstCode_[length];
                if (index >= symbols_.size()) return false;
                *symbol = symbols_[index];
                return true;
            }
        }
        return false;
    }

private:
    std::array<std::uint32_t, 16> firstCode_{};
    std::array<std::uint32_t, 16> firstSymbol_{};
    std::array<std::uint32_t, 16> count_{};
    std::vector<std::uint32_t> symbols_;
};

struct Rar5Tables {
    Rar5Huffman literalLength;
    Rar5Huffman distance;
    Rar5Huffman lowDistance;
    Rar5Huffman repeatedLength;
};

struct Rar5UnpackState {
    Rar5Tables tables;
    bool haveTables = false;
    bool tableExtendedDistances = false;
    std::array<std::uint64_t, 4> oldDistances{{0, 0, 0, 0}};
    std::uint64_t lastLength = 0;
    std::vector<std::uint8_t> history;
};

struct Rar5Filter {
    std::size_t offset;
    std::size_t length;
    std::uint32_t type;
    std::uint32_t channels;
};

bool readRar5FilterData(Rar5BitReader* bits, std::uint32_t* value)
{
    std::uint32_t count = 0;
    if (!bits || !value || !bits->read(2, &count)) return false;
    ++count;
    *value = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t byte = 0;
        if (!bits->read(8, &byte)) return false;
        *value |= byte << (i * 8U);
    }
    return true;
}

bool applyRar5Filters(std::vector<std::uint8_t>* output,
                      const std::vector<Rar5Filter>& filters)
{
    if (!output) return false;
    for (const Rar5Filter& filter : filters) {
        if (filter.offset > output->size() ||
            filter.length > output->size() - filter.offset) return false;
        std::uint8_t* data = output->data() + filter.offset;
        const std::size_t size = filter.length;
        if (filter.type == 1 || filter.type == 2) {
            const std::uint8_t second = filter.type == 2 ? 0xe9U : 0xe8U;
            for (std::size_t i = 0; i + 4 < size;) {
                const std::uint8_t opcode = data[i++];
                if (opcode != 0xe8U && opcode != second) continue;
                const std::uint32_t offset =
                    std::uint32_t(filter.offset + i) % 0x01000000U;
                const std::uint32_t address = readLe32(data + i);
                if (address & 0x80000000U) {
                    if (((address + offset) & 0x80000000U) == 0) {
                        const std::uint32_t adjusted = address + 0x01000000U;
                        for (unsigned byte = 0; byte < 4; ++byte)
                            data[i + byte] = std::uint8_t(adjusted >> (byte * 8U));
                    }
                } else if (address < 0x01000000U) {
                    const std::uint32_t adjusted = address - offset;
                    for (unsigned byte = 0; byte < 4; ++byte)
                        data[i + byte] = std::uint8_t(adjusted >> (byte * 8U));
                }
                i += 4;
            }
        } else if (filter.type == 3) {
            for (std::size_t i = 0; i + 3 < size; i += 4) {
                if (data[i + 3] != 0xeb) continue;
                const std::uint32_t address = std::uint32_t(data[i]) |
                    (std::uint32_t(data[i + 1]) << 8) |
                    (std::uint32_t(data[i + 2]) << 16);
                const std::uint32_t adjusted = address -
                    std::uint32_t((filter.offset + i) / 4);
                data[i] = std::uint8_t(adjusted);
                data[i + 1] = std::uint8_t(adjusted >> 8);
                data[i + 2] = std::uint8_t(adjusted >> 16);
            }
        } else if (filter.type == 0) {
            if (!filter.channels || filter.channels > 32) return false;
            std::vector<std::uint8_t> restored(size);
            std::size_t source = 0;
            for (std::uint32_t channel = 0;
                 channel < filter.channels && channel < size; ++channel) {
                std::uint8_t previous = 0;
                for (std::size_t dest = channel; dest < size;
                     dest += filter.channels) {
                    previous = std::uint8_t(previous - data[source++]);
                    restored[dest] = previous;
                }
            }
            std::copy(restored.begin(), restored.end(), data);
        } else {
            return false;
        }
    }
    return true;
}

bool readRar5Tables(Rar5BitReader* bits, Rar5Tables* tables,
                    bool extendedDistances)
{
    std::vector<std::uint8_t> bitLengths(20);
    for (std::size_t i = 0; i < bitLengths.size(); ++i) {
        std::uint32_t length = 0;
        if (!bits->read(4, &length)) return false;
        if (length == 15) {
            std::uint32_t zeros = 0;
            if (!bits->read(4, &zeros)) return false;
            if (!zeros) bitLengths[i] = 15;
            else {
                const std::size_t run = zeros + 2;
                if (run > bitLengths.size() - i) return false;
                i += run - 1;
            }
        } else bitLengths[i] = std::uint8_t(length);
    }
    Rar5Huffman bitLengthTree;
    if (!bitLengthTree.build(bitLengths)) return false;
    const std::size_t distanceCount = extendedDistances ? 80 : 64;
    const std::size_t tableSize = 306 + distanceCount + 16 + 44;
    std::vector<std::uint8_t> lengths;
    lengths.reserve(tableSize);
    while (lengths.size() < tableSize) {
        std::uint32_t symbol = 0;
        if (!bitLengthTree.decode(bits, &symbol)) return false;
        if (symbol < 16) lengths.push_back(std::uint8_t(symbol));
        else {
            std::uint32_t extra = 0;
            std::size_t count = 0;
            if (symbol < 18) {
                if (lengths.empty()) return false;
                if (!bits->read(symbol == 16 ? 3 : 7, &extra)) return false;
                count = extra + (symbol == 16 ? 3 : 11);
                if (count > tableSize - lengths.size()) return false;
                lengths.insert(lengths.end(), count, lengths.back());
            } else if (symbol < 20) {
                if (!bits->read(symbol == 18 ? 3 : 7, &extra)) return false;
                count = extra + (symbol == 18 ? 3 : 11);
                if (count > tableSize - lengths.size()) return false;
                lengths.insert(lengths.end(), count, 0);
            } else return false;
        }
    }
    std::vector<std::uint8_t> part(lengths.begin(), lengths.begin() + 306);
    if (!tables->literalLength.build(part)) return false;
    part.assign(lengths.begin() + 306,
                lengths.begin() + 306 + distanceCount);
    if (!tables->distance.build(part)) return false;
    part.assign(lengths.begin() + 306 + distanceCount,
                lengths.begin() + 322 + distanceCount);
    if (!tables->lowDistance.build(part)) return false;
    part.assign(lengths.begin() + 322 + distanceCount, lengths.end());
    if (!tables->repeatedLength.build(part)) return false;
    return true;
}

bool decodeRar5Member(const std::uint8_t* packed, std::size_t packedSize,
                      std::uint64_t unpackedSize, std::uint64_t dictionarySize,
                      bool extendedDistances, bool allowPadding, bool preserveHistory,
                      Rar5UnpackState* state,
                      std::vector<std::uint8_t>* output)
{
    if (!packed || !state || !output ||
        unpackedSize > std::uint64_t(output->max_size())) return false;
    output->clear();
    output->reserve(std::size_t(unpackedSize));
    std::vector<Rar5Filter> filters;
    std::size_t position = 0;
    while (position < packedSize) {
        if (packedSize - position < 3) return false;
        const std::uint8_t flags = packed[position++];
        const std::uint8_t savedChecksum = packed[position++];
        const unsigned byteCount = ((flags >> 3) & 3U) + 1;
        if (byteCount == 4 || byteCount > packedSize - position) return false;
        std::uint32_t blockSize = 0;
        for (unsigned i = 0; i < byteCount; ++i)
            blockSize |= std::uint32_t(packed[position++]) << (8U * i);
        const std::uint8_t checksum = std::uint8_t(0x5aU ^ flags ^ blockSize ^
            (blockSize >> 8) ^ (blockSize >> 16));
        if (!blockSize || checksum != savedChecksum || blockSize > packedSize - position)
            return false;
        Rar5BitReader bits(packed + position, blockSize);
        const std::size_t blockEnd = (std::size_t(blockSize) - 1) * 8 + (flags & 7U) + 1;
        if (flags & 0x80U) {
            if (!readRar5Tables(&bits, &state->tables, extendedDistances)) return false;
            state->haveTables = true;
            state->tableExtendedDistances = extendedDistances;
        }
        if (!state->haveTables || state->tableExtendedDistances != extendedDistances)
            return false;
        while (bits.position() < blockEnd && output->size() < unpackedSize) {
            std::uint32_t symbol = 0;
            if (!state->tables.literalLength.decode(&bits, &symbol)) return false;
            if (symbol < 256) output->push_back(std::uint8_t(symbol));
            else {
                std::uint64_t length = 0;
                std::uint64_t distance = 0;
                if (symbol == 256) {
                    std::uint32_t blockStart = 0;
                    std::uint32_t blockLength = 0;
                    std::uint32_t type = 0;
                    if (!readRar5FilterData(&bits, &blockStart) ||
                        !readRar5FilterData(&bits, &blockLength) ||
                        !bits.read(3, &type)) return false;
                    std::uint32_t channels = 0;
                    if (type == 0) {
                        if (!bits.read(5, &channels)) return false;
                        ++channels;
                    }
                    if (!blockLength || blockLength > 0x400000U ||
                        blockStart > unpackedSize - output->size() ||
                        filters.size() >= 8192) return false;
                    filters.push_back({output->size() + blockStart, blockLength,
                                       type, channels});
                    continue;
                }
                if (symbol == 257) {
                    if (!state->lastLength) return false;
                    length = state->lastLength;
                    distance = state->oldDistances[0];
                } else if (symbol < 262) {
                    const unsigned index = symbol - 258;
                    distance = state->oldDistances[index];
                    for (unsigned i = index; i > 0; --i)
                        state->oldDistances[i] = state->oldDistances[i - 1];
                    state->oldDistances[0] = distance;
                    std::uint32_t slot = 0;
                    if (!state->tables.repeatedLength.decode(&bits, &slot)) return false;
                    std::uint32_t extra = 0;
                    const unsigned extraBits = slot < 8 ? 0 : slot / 4 - 1;
                    length = slot < 8 ? 2 + slot : 2 + ((4U | (slot & 3U)) << extraBits);
                    if (extraBits && (!bits.read(extraBits, &extra))) return false;
                    length += extra;
                } else {
                    const std::uint32_t slot = symbol - 262;
                    const unsigned lengthBits = slot < 8 ? 0 : slot / 4 - 1;
                    std::uint32_t extraLength = 0;
                    length = slot < 8 ? 2 + slot : 2 + ((4U | (slot & 3U)) << lengthBits);
                    if (lengthBits && !bits.read(lengthBits, &extraLength)) return false;
                    length += extraLength;
                    std::uint32_t distanceSlot = 0;
                    if (!state->tables.distance.decode(&bits, &distanceSlot)) return false;
                    unsigned distanceBits = 0;
                    distance = 1;
                    if (distanceSlot < 4) distance += distanceSlot;
                    else {
                        distanceBits = distanceSlot / 2 - 1;
                        distance += std::uint64_t(2U | (distanceSlot & 1U)) << distanceBits;
                        if (distanceBits >= 4) {
                            std::uint64_t high = 0;
                            std::uint32_t low = 0;
                            if (distanceBits > 4 && !bits.read64(distanceBits - 4, &high)) return false;
                            if (!state->tables.lowDistance.decode(&bits, &low)) return false;
                            distance += (std::uint64_t(high) << 4) + low;
                        } else if (distanceBits) {
                            std::uint32_t low = 0;
                            if (!bits.read(distanceBits, &low)) return false;
                            distance += low;
                        }
                    }
                    if (distance > 0x100) ++length;
                    if (distance > 0x2000) ++length;
                    if (distance > 0x40000) ++length;
                    for (unsigned i = 3; i > 0; --i)
                        state->oldDistances[i] = state->oldDistances[i - 1];
                    state->oldDistances[0] = distance;
                }
                const std::uint64_t available =
                    std::uint64_t(state->history.size()) + output->size();
                if (!distance || distance > dictionarySize || distance > available ||
                    length > unpackedSize - output->size()) {
                    return false;
                }
                state->lastLength = length;
                const std::size_t outputStart = output->size();
                const std::size_t matchLength = std::size_t(length);
                const std::size_t seedLength = std::size_t(std::min(length, distance));
                const std::uint64_t source = std::uint64_t(state->history.size()) +
                    outputStart - distance;
                output->resize(outputStart + matchLength);
                std::uint8_t* destination = output->data() + outputStart;
                std::size_t copied = 0;
                if (source < state->history.size()) {
                    const std::size_t historyOffset = std::size_t(source);
                    copied = std::min(seedLength,
                        state->history.size() - historyOffset);
                    std::memcpy(destination, state->history.data() + historyOffset, copied);
                    if (copied < seedLength) {
                        const std::size_t outputOffset = historyOffset + copied -
                            state->history.size();
                        std::memcpy(destination + copied, output->data() + outputOffset,
                                    seedLength - copied);
                    }
                } else {
                    const std::size_t outputOffset =
                        std::size_t(source - state->history.size());
                    std::memcpy(destination, output->data() + outputOffset, seedLength);
                    copied = seedLength;
                }
                while (copied < matchLength) {
                    const std::size_t amount =
                        std::min(copied, matchLength - copied);
                    std::memcpy(destination + copied, destination, amount);
                    copied += amount;
                }
            }
        }
        if (bits.position() > blockEnd) return false;
        position += blockSize;
        if (flags & 0x40U) {
            if (output->size() != unpackedSize || position > packedSize) return false;
            const std::size_t padding = packedSize - position;
            if ((!allowPadding && padding) || padding > 15 ||
                std::any_of(packed + position, packed + packedSize,
                            [](std::uint8_t value) { return value != 0; })) return false;
            if (preserveHistory) {
                state->history.insert(state->history.end(), output->begin(), output->end());
                if (state->history.size() > dictionarySize) {
                    const std::size_t keep = std::size_t(dictionarySize);
                    state->history.erase(state->history.begin(), state->history.end() - keep);
                }
            }
            return applyRar5Filters(output, filters);
        }
    }
    return false;
}
