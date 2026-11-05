#include "SevenZipModule.h"
#include "Compat.h"
#include "DeflateDecoder.h"
#include "../compression/Crc32.h"
#include "../compression/Bcj2Decoder.h"
#include "../compression/SevenZipAesDecoder.h"
#include "../compression/peare/Bzip2Decoder.h"
#include "../compression/LzmaDecoder.h"
#include "../compression/third_party/lzma/Bra.h"
#include "../compression/third_party/lzma/Delta.h"
#include "../fs/QFileByteStore.h"

#include <QFile>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace peare {
namespace {

using compression::crc32;

std::uint32_t crc32Parallel(const std::uint8_t* data, std::size_t size)
{
    if (size < 32 * 1024 * 1024 || !data) return crc32(data, size);
    const unsigned available = std::thread::hardware_concurrency();
    const unsigned workers = std::min<unsigned>(8, available ? available : 2);
    if (workers < 2) return crc32(data, size);

    const auto matrixTimes = [](const std::uint32_t* matrix, std::uint32_t vector) {
        std::uint32_t sum = 0;
        unsigned index = 0;
        while (vector) {
            if (vector & 1U) sum ^= matrix[index];
            vector >>= 1;
            ++index;
        }
        return sum;
    };
    const auto matrixSquare = [&matrixTimes](std::uint32_t* square,
                                             const std::uint32_t* matrix) {
        for (unsigned i = 0; i < 32; ++i)
            square[i] = matrixTimes(matrix, matrix[i]);
    };
    const auto combine = [&matrixSquare, &matrixTimes](std::uint32_t first,
                                                       std::uint32_t second,
                                                       std::size_t secondSize) {
        if (!secondSize) return first;
        std::uint32_t even[32]{};
        std::uint32_t odd[32]{};
        odd[0] = 0xEDB88320U;
        std::uint32_t row = 1;
        for (unsigned i = 1; i < 32; ++i) {
            odd[i] = row;
            row <<= 1;
        }
        matrixSquare(even, odd);
        matrixSquare(odd, even);
        do {
            matrixSquare(even, odd);
            if (secondSize & 1U) first = matrixTimes(even, first);
            secondSize >>= 1;
            if (!secondSize) break;
            matrixSquare(odd, even);
            if (secondSize & 1U) first = matrixTimes(odd, first);
            secondSize >>= 1;
        } while (secondSize);
        return first ^ second;
    };

    const std::size_t baseSize = size / workers;
    const std::size_t remainder = size % workers;
    try {
        std::vector<std::future<std::uint32_t>> tasks;
        tasks.reserve(workers - 1);
        std::uint32_t result = 0;
        std::size_t offset = 0;
        for (unsigned worker = 0; worker < workers; ++worker) {
            const std::size_t length = baseSize + (worker < remainder ? 1 : 0);
            if (worker == 0) {
                result = crc32(data, length);
            } else {
                tasks.push_back(std::async(std::launch::async, [data, offset, length] {
                    return crc32(data + offset, length);
                }));
            }
            offset += length;
        }
        for (unsigned worker = 1; worker < workers; ++worker) {
            const std::size_t length = baseSize + (worker < remainder ? 1 : 0);
            result = combine(result, tasks[worker - 1].get(), length);
        }
        return result;
    } catch (...) {
        return crc32(data, size);
    }
}

constexpr std::size_t kStartHeaderSize = 32;
constexpr std::size_t kMaximumArchiveSize =
    std::size_t(std::numeric_limits<std::size_t>::max());
constexpr std::size_t kMaximumDecodedSize =
    std::size_t(std::numeric_limits<std::size_t>::max());
constexpr std::size_t kMaximumFiles = 1000000;
const char kSignature[] = {'7', 'z', char(0xBC), char(0xAF), char(0x27), char(0x1C)};

class Cursor {
public:
    Cursor(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool byte(std::uint8_t* value)
    {
        if (!value || position_ >= size_) return false;
        *value = data_[position_++];
        return true;
    }

    bool number(std::uint64_t* value)
    {
        std::uint8_t first = 0;
        if (!value || !byte(&first)) return false;
        std::uint64_t result = 0;
        unsigned extra = 0;
        for (; extra < 8 && (first & (0x80U >> extra)); ++extra) {}
        if (extra == 8) {
            for (unsigned i = 0; i < 8; ++i) {
                std::uint8_t next = 0;
                if (!byte(&next)) return false;
                result |= std::uint64_t(next) << (8 * i);
            }
            *value = result;
            return true;
        }
        const unsigned highBits = 7 - extra;
        if (highBits) result |= std::uint64_t(first & ((1U << highBits) - 1U)) << (8 * extra);
        for (unsigned i = 0; i < extra; ++i) {
            std::uint8_t next = 0;
            if (!byte(&next)) return false;
            result |= std::uint64_t(next) << (8 * i);
        }
        *value = result;
        return true;
    }

    bool bytes(std::size_t count, const std::uint8_t** result)
    {
        if (!result || count > remaining()) return false;
        *result = data_ + position_;
        position_ += count;
        return true;
    }

    bool skip(std::size_t count)
    {
        const std::uint8_t* ignored = nullptr;
        return bytes(count, &ignored);
    }

    std::size_t position() const { return position_; }
    std::size_t remaining() const { return size_ - position_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_ = 0;
};

struct Digest {
    bool defined = false;
    std::uint32_t value = 0;

    Digest() = default;
    Digest(bool isDefined, std::uint32_t digestValue)
        : defined(isDefined), value(digestValue) {}
};

struct PackInfo {
    std::uint64_t position = 0;
    std::vector<std::uint64_t> sizes;
    std::vector<Digest> digests;
};

struct CoderDescription {
    QByteArray method;
    QByteArray properties;
    std::size_t inputCount = 1;
    std::size_t outputCount = 1;
    std::size_t inputBase = 0;
    std::size_t outputBase = 0;
};

struct BindPair {
    std::size_t inputIndex = 0;
    std::size_t outputIndex = 0;
};

struct Folder {
    std::vector<CoderDescription> coders;
    std::vector<BindPair> binds;
    std::vector<std::size_t> packInputIndices;
    std::vector<std::uint64_t> outputSizes;
    std::size_t finalOutputIndex = 0;
    std::size_t packStreamCount = 0;
    std::uint64_t unpackSize = 0;
    Digest digest;
};

struct PackedStream {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    PackedStream() = default;
    PackedStream(const std::uint8_t* bytes, std::size_t byteCount)
        : data(bytes), size(byteCount) {}
};

struct StreamsInfo {
    PackInfo pack;
    std::vector<Folder> folders;
    std::vector<std::uint64_t> substreamCounts;
    std::vector<std::uint64_t> substreamSizes;
    std::vector<Digest> substreamDigests;
};

struct FileInfo {
    QString name;
    bool emptyStream = false;
    bool emptyFile = false;
    bool externalName = false;
    std::uint64_t nameStreamIndex = 0;
};

class MappedArchive final {
public:
    explicit MappedArchive(const QString& path) : file_(path) {
        if (!file_.open(QIODevice::ReadOnly)) {
            error_ = file_.errorString();
            return;
        }
        size_ = file_.size();
        if (size_ <= 0) {
            error_ = QStringLiteral("Empty 7-Zip archive");
            return;
        }
        if (std::uint64_t(size_) > std::uint64_t(kMaximumArchiveSize)) {
            error_ = QStringLiteral("7-Zip archive exceeds the addressable size limit");
            return;
        }
        mapping_ = file_.map(0, size_);
        if (!mapping_) error_ = file_.errorString();
    }

    ~MappedArchive() {
        if (mapping_) file_.unmap(mapping_);
    }

    bool isValid() const { return mapping_ != nullptr; }
    qint64 size() const { return size_; }
    const char* data() const { return reinterpret_cast<const char*>(mapping_); }
    const QString& error() const { return error_; }

private:
    QFile file_;
    uchar* mapping_ = nullptr;
    qint64 size_ = 0;
    QString error_;
};

bool decodeAdditionalStreams(const std::uint8_t* archive, std::size_t archiveSize,
                             const StreamsInfo& streams,
                             const QString& password,
                             std::vector<std::vector<std::uint8_t>>* substreams);

std::uint32_t readLe32(const std::uint8_t* data)
{
    return qFromLittleEndian<std::uint32_t>(data);
}

bool readBitVector(Cursor& cursor, std::size_t count, std::vector<bool>* values)
{
    if (!values) return false;
    values->assign(count, false);
    std::uint8_t current = 0;
    unsigned available = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!available) {
            if (!cursor.byte(&current)) return false;
            available = 8;
        }
        (*values)[i] = (current & 0x80U) != 0;
        current <<= 1;
        --available;
    }
    return true;
}

bool readDigests(Cursor& cursor, std::size_t count, std::vector<Digest>* digests)
{
    std::uint8_t allDefined = 0;
    std::vector<bool> defined;
    if (!digests || !cursor.byte(&allDefined) || allDefined > 1) return false;
    if (!allDefined && !readBitVector(cursor, count, &defined)) return false;
    digests->assign(count, {});
    for (std::size_t i = 0; i < count; ++i) {
        const bool hasDigest = allDefined || defined[i];
        if (!hasDigest) continue;
        const std::uint8_t* bytes = nullptr;
        if (!cursor.bytes(4, &bytes)) return false;
        (*digests)[i] = {true, readLe32(bytes)};
    }
    return true;
}

bool skipProperties(Cursor& cursor)
{
    for (;;) {
        std::uint8_t property = 0;
        if (!cursor.byte(&property)) return false;
        if (property == 0) return true;
        std::uint64_t size = 0;
        if (!cursor.number(&size) || size > cursor.remaining()) return false;
        if (!cursor.skip(std::size_t(size))) return false;
    }
}

bool parsePackInfo(Cursor& cursor, PackInfo* pack)
{
    std::uint64_t count = 0;
    if (!pack || !cursor.number(&pack->position) || !cursor.number(&count) ||
        count > kMaximumFiles)
        return false;
    std::uint8_t property = 0;
    if (!cursor.byte(&property) || property != 9) return false;
    pack->sizes.resize(std::size_t(count));
    for (std::uint64_t& size : pack->sizes)
        if (!cursor.number(&size)) return false;
    for (;;) {
        if (!cursor.byte(&property)) return false;
        if (property == 0) return true;
        if (property == 10) {
            if (!readDigests(cursor, pack->sizes.size(), &pack->digests)) return false;
        } else {
            return false;
        }
    }
}

bool isCodecMethod(const QByteArray& method)
{
    return method == QByteArray(1, char(0)) ||
        method == QByteArray::fromHex("040108") ||
        method == QByteArray::fromHex("040109") ||
        method == QByteArray::fromHex("040202") ||
        method == QByteArray::fromHex("030101") ||
        method == QByteArray::fromHex("21") ||
        method == QByteArray::fromHex("030401");
}

bool isFilterMethod(const QByteArray& method)
{
    return method == QByteArray::fromHex("03") ||
        method == QByteArray::fromHex("020302") ||
        method == QByteArray::fromHex("020304") ||
        method == QByteArray::fromHex("03030103") ||
        method == QByteArray::fromHex("03030205") ||
        method == QByteArray::fromHex("03030401") ||
        method == QByteArray::fromHex("03030501") ||
        method == QByteArray::fromHex("03030701") ||
        method == QByteArray::fromHex("03030805") ||
        method == QByteArray::fromHex("0a") ||
        method == QByteArray::fromHex("0b");
}

bool parseFolder(Cursor& cursor, Folder* folder)
{
    constexpr std::size_t kMaximumCoders = 16;
    constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();
    std::uint64_t coderCount64 = 0;
    if (!folder || !cursor.number(&coderCount64) || coderCount64 == 0 ||
        coderCount64 > kMaximumCoders) return false;
    const std::size_t coderCount = std::size_t(coderCount64);
    std::vector<CoderDescription> coders(coderCount);
    std::size_t totalInputs = 0;
    std::size_t totalOutputs = 0;
    for (CoderDescription& description : coders) {
        std::uint8_t flags = 0;
        if (!cursor.byte(&flags) || (flags & 0xC0U) != 0 || (flags & 0x0FU) == 0)
            return false;
        const std::size_t idSize = flags & 0x0FU;
        const std::uint8_t* id = nullptr;
        if (!cursor.bytes(idSize, &id)) return false;
        description.method = QByteArray(reinterpret_cast<const char*>(id), int(idSize));
        if (flags & 0x10U) {
            std::uint64_t inputs = 0;
            std::uint64_t outputs = 0;
            if (!cursor.number(&inputs) || !cursor.number(&outputs) || !inputs || !outputs ||
                inputs > 64 || outputs > 64) return false;
            description.inputCount = std::size_t(inputs);
            description.outputCount = std::size_t(outputs);
        }
        if (description.inputCount > 64 - totalInputs ||
            description.outputCount > 64 - totalOutputs) return false;
        description.inputBase = totalInputs;
        description.outputBase = totalOutputs;
        totalInputs += description.inputCount;
        totalOutputs += description.outputCount;
        if (flags & 0x20U) {
            std::uint64_t propertiesSize = 0;
            const std::uint8_t* bytes = nullptr;
            if (!cursor.number(&propertiesSize) || propertiesSize > cursor.remaining() ||
                !cursor.bytes(std::size_t(propertiesSize), &bytes)) return false;
            description.properties = QByteArray(reinterpret_cast<const char*>(bytes),
                                                int(propertiesSize));
        }
    }
    if (!totalInputs || !totalOutputs) return false;
    const std::size_t bindCount = totalOutputs - 1;
    if (bindCount > totalInputs) return false;
    std::vector<BindPair> binds(bindCount);
    std::vector<bool> boundInputs(totalInputs, false);
    std::vector<bool> boundOutputs(totalOutputs, false);
    for (BindPair& bind : binds) {
        std::uint64_t input = 0;
        std::uint64_t output = 0;
        if (!cursor.number(&input) || !cursor.number(&output) ||
            input >= totalInputs || output >= totalOutputs ||
            boundInputs[std::size_t(input)] || boundOutputs[std::size_t(output)])
            return false;
        bind.inputIndex = std::size_t(input);
        bind.outputIndex = std::size_t(output);
        boundInputs[bind.inputIndex] = true;
        boundOutputs[bind.outputIndex] = true;
    }
    std::vector<std::size_t> unboundInputs;
    std::size_t finalOutput = kNoIndex;
    for (std::size_t i = 0; i < totalInputs; ++i)
        if (!boundInputs[i]) unboundInputs.push_back(i);
    for (std::size_t i = 0; i < totalOutputs; ++i)
        if (!boundOutputs[i]) {
            if (finalOutput != kNoIndex) return false;
            finalOutput = i;
        }
    if (finalOutput == kNoIndex || unboundInputs.empty()) return false;

    std::vector<std::size_t> packInputs = unboundInputs;
    if (unboundInputs.size() > 1) {
        for (std::size_t& input : packInputs) {
            std::uint64_t value = 0;
            if (!cursor.number(&value) || value >= totalInputs || boundInputs[std::size_t(value)])
                return false;
            input = std::size_t(value);
        }
        std::vector<bool> seen(totalInputs, false);
        for (std::size_t input : packInputs) {
            if (seen[input]) return false;
            seen[input] = true;
        }
    }
    folder->coders = std::move(coders);
    folder->binds = std::move(binds);
    folder->packInputIndices = std::move(packInputs);
    folder->outputSizes.resize(totalOutputs);
    folder->packStreamCount = folder->packInputIndices.size();
    folder->finalOutputIndex = finalOutput;
    return true;
}

bool parseUnpackInfo(Cursor& cursor, StreamsInfo* streams,
                     const std::vector<std::vector<std::uint8_t>>* externalSubstreams)
{
    std::uint8_t property = 0;
    std::uint64_t folderCount = 0;
    std::uint8_t external = 0;
    if (!streams || !cursor.byte(&property) || property != 11 ||
        !cursor.number(&folderCount) || folderCount > kMaximumFiles ||
        !cursor.byte(&external) || external > 1)
        return false;
    if (external) {
        std::uint64_t streamIndex = 0;
        if (!externalSubstreams || !cursor.number(&streamIndex) ||
            streamIndex >= externalSubstreams->size()) return false;
        const auto& data = (*externalSubstreams)[std::size_t(streamIndex)];
        Cursor externalCursor(data.data(), data.size());
        streams->folders.resize(std::size_t(folderCount));
        for (Folder& folder : streams->folders)
            if (!parseFolder(externalCursor, &folder)) return false;
        if (externalCursor.remaining()) return false;
    } else {
        streams->folders.resize(std::size_t(folderCount));
        for (Folder& folder : streams->folders)
            if (!parseFolder(cursor, &folder)) return false;
    }
    if (!cursor.byte(&property) || property != 12) return false;
    for (Folder& folder : streams->folders) {
        std::uint64_t totalOutputSize = 0;
        for (std::uint64_t& size : folder.outputSizes) {
            if (!cursor.number(&size) || size > kMaximumDecodedSize ||
                size > kMaximumDecodedSize - totalOutputSize) return false;
            totalOutputSize += size;
        }
        if (folder.finalOutputIndex >= folder.outputSizes.size()) return false;
        folder.unpackSize = folder.outputSizes[folder.finalOutputIndex];
    }
    for (;;) {
        if (!cursor.byte(&property)) return false;
        if (property == 0) return true;
        if (property == 10) {
            std::vector<Digest> digests;
            if (!readDigests(cursor, streams->folders.size(), &digests)) return false;
            for (std::size_t i = 0; i < digests.size(); ++i)
                streams->folders[i].digest = digests[i];
        } else {
            return false;
        }
    }
}

bool parseSubStreamsInfo(Cursor& cursor, StreamsInfo* streams)
{
    std::uint8_t property = 0;
    std::vector<std::uint64_t> substreamCounts(streams->folders.size(), 1);
    bool sawSizes = false;
    bool sawDigests = false;
    for (;;) {
        if (!cursor.byte(&property)) return false;
        if (property == 0) break;
        if (property == 13) {
            for (std::uint64_t& count : substreamCounts)
                if (!cursor.number(&count) || count > kMaximumFiles) return false;
            std::uint64_t total = 0;
            for (std::uint64_t count : substreamCounts) {
                if (count > kMaximumFiles - total) return false;
                total += count;
            }
            continue;
        }
        if (property == 9) {
            if (sawSizes) return false;
            sawSizes = true;
            for (std::size_t i = 0; i < substreamCounts.size(); ++i) {
                const std::uint64_t count = substreamCounts[i];
                if (!count) continue;
                std::uint64_t folderSize = 0;
                for (std::uint64_t j = 1; j < count; ++j) {
                    std::uint64_t size = 0;
                    if (!cursor.number(&size) || size > streams->folders[i].unpackSize - folderSize)
                        return false;
                    folderSize += size;
                    streams->substreamSizes.push_back(size);
                }
                streams->substreamSizes.push_back(streams->folders[i].unpackSize - folderSize);
            }
            continue;
        }
        if (property == 10) {
            if (sawDigests) return false;
            sawDigests = true;
            std::size_t digestCount = 0;
            for (std::size_t i = 0; i < substreamCounts.size(); ++i)
                if (substreamCounts[i]) {
                    if (substreamCounts[i] > kMaximumFiles - digestCount) return false;
                    digestCount += std::size_t(substreamCounts[i]);
                    if (substreamCounts[i] == 1 && streams->folders[i].digest.defined)
                        --digestCount;
                }
            std::vector<Digest> read;
            if (!readDigests(cursor, digestCount, &read)) return false;
            std::size_t readIndex = 0;
            streams->substreamDigests.reserve(digestCount + substreamCounts.size());
            for (std::size_t i = 0; i < substreamCounts.size(); ++i) {
                for (std::uint64_t j = 0; j < substreamCounts[i]; ++j) {
                    if (substreamCounts[i] == 1 && streams->folders[i].digest.defined)
                        streams->substreamDigests.push_back(streams->folders[i].digest);
                    else
                        streams->substreamDigests.push_back(read[readIndex++]);
                }
            }
            continue;
        }
        return false;
    }
    if (!sawSizes) {
        for (std::size_t i = 0; i < substreamCounts.size(); ++i) {
            if (substreamCounts[i] > 1) return false;
            if (substreamCounts[i]) streams->substreamSizes.push_back(streams->folders[i].unpackSize);
        }
    }
    std::uint64_t totalSubstreams = 0;
    for (std::uint64_t count : substreamCounts) {
        if (count > kMaximumFiles - totalSubstreams) return false;
        totalSubstreams += count;
    }
    if (streams->substreamSizes.size() != totalSubstreams) return false;
    if (!sawDigests)
        for (std::size_t i = 0; i < substreamCounts.size(); ++i)
            if (substreamCounts[i] == 1) streams->substreamDigests.push_back(streams->folders[i].digest);
            else streams->substreamDigests.insert(streams->substreamDigests.end(),
                std::size_t(substreamCounts[i]), {});
    if (streams->substreamDigests.size() != totalSubstreams) return false;
    streams->substreamCounts = std::move(substreamCounts);
    return true;
}

bool parseStreamsInfo(Cursor& cursor, StreamsInfo* streams,
                      const std::vector<std::vector<std::uint8_t>>* externalSubstreams = nullptr)
{
    bool sawPack = false;
    bool sawUnpack = false;
    for (;;) {
        std::uint8_t property = 0;
        if (!cursor.byte(&property)) return false;
        if (property == 0) {
            if (!sawPack || !sawUnpack) return false;
            std::size_t packCount = 0;
            for (const Folder& folder : streams->folders) {
                if (folder.packStreamCount > kMaximumFiles - packCount) return false;
                packCount += folder.packStreamCount;
            }
            if (streams->pack.sizes.size() != packCount) return false;
            if (streams->substreamCounts.empty()) {
                for (const Folder& folder : streams->folders) {
                    streams->substreamCounts.push_back(1);
                    streams->substreamSizes.push_back(folder.unpackSize);
                    streams->substreamDigests.push_back(folder.digest);
                }
            }
            std::uint64_t total = 0;
            for (std::uint64_t count : streams->substreamCounts) {
                if (count > kMaximumFiles - total) return false;
                total += count;
            }
            return streams->substreamSizes.size() == total &&
                   streams->substreamDigests.size() == total;
        }
        if (property == 6) {
            if (sawPack) return false;
            sawPack = true;
            if (!parsePackInfo(cursor, &streams->pack)) return false;
        } else if (property == 7) {
            if (sawUnpack || !parseUnpackInfo(cursor, streams, externalSubstreams)) return false;
            sawUnpack = true;
        } else if (property == 8) {
            if (!sawUnpack || !parseSubStreamsInfo(cursor, streams)) return false;
        } else {
            return false;
        }
    }
}

bool parseFilesInfo(Cursor& cursor, std::vector<FileInfo>* files)
{
    std::uint64_t count = 0;
    if (!files || !cursor.number(&count) || count > kMaximumFiles) return false;
    files->assign(std::size_t(count), {});
    for (;;) {
        std::uint8_t property = 0;
        if (!cursor.byte(&property)) return false;
        if (property == 0) return true;
        std::uint64_t size = 0;
        const std::uint8_t* data = nullptr;
        if (!cursor.number(&size) || size > cursor.remaining() ||
            !cursor.bytes(std::size_t(size), &data)) return false;
        Cursor propertyCursor(data, std::size_t(size));
        if (property == 14) {
            std::vector<bool> empty;
            if (!readBitVector(propertyCursor, files->size(), &empty)) return false;
            for (std::size_t i = 0; i < files->size(); ++i) (*files)[i].emptyStream = empty[i];
        } else if (property == 15) {
            std::size_t emptyCount = 0;
            for (const FileInfo& file : *files) if (file.emptyStream) ++emptyCount;
            std::vector<bool> empty;
            if (!readBitVector(propertyCursor, emptyCount, &empty)) return false;
            std::size_t index = 0;
            for (FileInfo& file : *files)
                if (file.emptyStream) file.emptyFile = empty[index++];
        } else if (property == 17) {
            std::uint8_t external = 0;
            if (!propertyCursor.byte(&external) || external > 1) return false;
            if (external) {
                std::uint64_t streamIndex = 0;
                if (!propertyCursor.number(&streamIndex)) return false;
                for (FileInfo& file : *files) {
                    file.externalName = true;
                    file.nameStreamIndex = streamIndex;
                }
            } else {
                for (FileInfo& file : *files) {
                    QVector<ushort> name;
                    for (;;) {
                        const std::uint8_t* unit = nullptr;
                        if (!propertyCursor.bytes(2, &unit)) return false;
                        const ushort value = qFromLittleEndian<ushort>(unit);
                        if (!value) break;
                        name.push_back(value);
                    }
                    file.name = QString::fromUtf16(name.constData(), name.size());
                }
            }
        }
        if ((property == 14 || property == 15 || property == 17) &&
            propertyCursor.remaining()) return false;
    }
}

QStringList safePath(QString name)
{
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    QStringList parts;
    const QString invalid = QStringLiteral("<>:\"|?*");
    for (QString part : name.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QStringLiteral(".")) continue;
        if (part == QStringLiteral("..")) part = QStringLiteral("_");
        for (QChar& c : part)
            if (c.unicode() < 0x20 || invalid.contains(c)) c = QLatin1Char('_');
        if (!part.isEmpty()) parts.push_back(part);
    }
    return parts;
}

bool decodeCodec(const QByteArray& method, const QByteArray& properties,
                 std::uint64_t unpackSize, const std::uint8_t* packed,
                 std::size_t packedSize, std::vector<std::uint8_t>* output)
{
    if (!output || unpackSize > kMaximumDecodedSize ||
        unpackSize > std::numeric_limits<std::size_t>::max() || (!packed && packedSize))
        return false;
    const std::size_t expected = std::size_t(unpackSize);
    std::vector<std::uint8_t> decoded;
    if (method == QByteArray(1, char(0))) {
        if (packedSize != expected) return false;
        if (packedSize) decoded.assign(packed, packed + packedSize);
    } else if (method == QByteArray::fromHex("040108")) {
        if (!compression::inflateRawExact(packed, packedSize, expected, nullptr, &decoded))
            return false;
    } else if (method == QByteArray::fromHex("040109")) {
        if (!compression::inflateRawDeflate64Exact(packed, packedSize, expected, &decoded))
            return false;
    } else if (method == QByteArray::fromHex("040202")) {
        if (!compression::decompressBzip2(packed, packedSize, expected, false, &decoded))
            return false;
    } else if (method == QByteArray::fromHex("030101")) {
        if (!compression::decodeLzmaRaw(packed, packedSize,
            reinterpret_cast<const std::uint8_t*>(properties.constData()),
            std::size_t(properties.size()), expected, &decoded)) return false;
    } else if (method == QByteArray::fromHex("21")) {
        if (!compression::decodeLzma2Raw(packed, packedSize,
            reinterpret_cast<const std::uint8_t*>(properties.constData()),
            std::size_t(properties.size()), expected, &decoded)) return false;
    } else if (method == QByteArray::fromHex("030401")) {
        if (!compression::decodePpmd7Raw(packed, packedSize,
            reinterpret_cast<const std::uint8_t*>(properties.constData()),
            std::size_t(properties.size()), expected, &decoded)) return false;
    } else {
        return false;
    }
    if (decoded.size() != expected) return false;
    output->swap(decoded);
    return true;
}

bool applyFilter(const QByteArray& filter, const QByteArray& properties,
                 std::vector<std::uint8_t>* data)
{
    if (!data) return false;
    std::uint8_t* buffer = data->empty() ? nullptr : data->data();
    const std::size_t size = data->size();
    if (filter == QByteArray::fromHex("03")) {
        if (properties.size() != 1) return false;
        std::uint8_t state[DELTA_STATE_SIZE];
        Delta_Init(state);
        Delta_Decode(state, std::uint8_t(properties[0]) + 1, buffer, size);
    } else if (filter == QByteArray::fromHex("020302") ||
               filter == QByteArray::fromHex("020304")) {
        if (!properties.isEmpty()) return false;
        const std::size_t wordSize = filter.endsWith(char(2)) ? 2 : 4;
        for (std::size_t offset = 0; offset + wordSize <= size; offset += wordSize)
            std::reverse(data->begin() + std::ptrdiff_t(offset),
                         data->begin() + std::ptrdiff_t(offset + wordSize));
    } else if (filter == QByteArray::fromHex("03030103")) {
        if (!properties.isEmpty()) return false;
        UInt32 state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
        z7_BranchConvSt_X86_Dec(buffer, size, 0, &state);
    } else if (filter == QByteArray::fromHex("03030205")) {
        if (!properties.isEmpty()) return false;
        z7_BranchConv_PPC_Dec(buffer, size, 0);
    } else if (filter == QByteArray::fromHex("03030401")) {
        if (!properties.isEmpty()) return false;
        z7_BranchConv_IA64_Dec(buffer, size, 0);
    } else if (filter == QByteArray::fromHex("03030501")) {
        if (!properties.isEmpty()) return false;
        z7_BranchConv_ARM_Dec(buffer, size, 0);
    } else if (filter == QByteArray::fromHex("03030701")) {
        if (!properties.isEmpty()) return false;
        z7_BranchConv_ARMT_Dec(buffer, size, 0);
    } else if (filter == QByteArray::fromHex("03030805")) {
        if (!properties.isEmpty()) return false;
        z7_BranchConv_SPARC_Dec(buffer, size, 0);
    } else if (filter == QByteArray::fromHex("0a")) {
        UInt32 pc = 0;
        if (properties.size() == 4)
            pc = qFromLittleEndian<UInt32>(
                reinterpret_cast<const uchar*>(properties.constData()));
        else if (!properties.isEmpty())
            return false;
        if (pc & 3) return false;
        z7_BranchConv_ARM64_Dec(buffer, size, pc);
    } else if (filter == QByteArray::fromHex("0b")) {
        UInt32 pc = 0;
        if (properties.size() == 4)
            pc = qFromLittleEndian<UInt32>(
                reinterpret_cast<const uchar*>(properties.constData()));
        else if (!properties.isEmpty())
            return false;
        if (pc & 1) return false;
        z7_BranchConv_RISCV_Dec(buffer, size, pc);
    } else {
        return false;
    }
    return true;
}

bool decodeFolder(const Folder& folder, const std::vector<PackedStream>& packed,
                  const QString& password, std::vector<std::uint8_t>* output,
                  ModuleOpenError* openError)
{
    if (!output || packed.size() != folder.packStreamCount || folder.coders.empty() ||
        folder.outputSizes.empty()) return false;
    constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();
    std::vector<std::vector<std::uint8_t>> outputs(folder.outputSizes.size());
    std::vector<bool> outputReady(folder.outputSizes.size(), false);
    std::vector<bool> coderDone(folder.coders.size(), false);
    for (std::size_t i = 0; i < packed.size(); ++i) {
        if (folder.packInputIndices[i] >= 64 || (!packed[i].data && packed[i].size))
            return false;
    }
    std::size_t completedCoders = 0;
    while (completedCoders < folder.coders.size()) {
        bool progressed = false;
        for (std::size_t coderIndex = 0; coderIndex < folder.coders.size(); ++coderIndex) {
            if (coderDone[coderIndex]) continue;
            const CoderDescription& coder = folder.coders[coderIndex];
            std::vector<std::vector<std::uint8_t>> inputBuffers(coder.inputCount);
            std::vector<const std::uint8_t*> inputData(coder.inputCount, nullptr);
            std::vector<std::size_t> inputSizes(coder.inputCount, 0);
            bool ready = true;
            for (std::size_t localInput = 0; localInput < coder.inputCount; ++localInput) {
                const std::size_t inputIndex = coder.inputBase + localInput;
                std::size_t sourceOutput = kNoIndex;
                for (const BindPair& bind : folder.binds)
                    if (bind.inputIndex == inputIndex) sourceOutput = bind.outputIndex;
                if (sourceOutput != kNoIndex) {
                    if (!outputReady[sourceOutput]) { ready = false; break; }
                    inputData[localInput] = outputs[sourceOutput].empty()
                        ? nullptr : outputs[sourceOutput].data();
                    inputSizes[localInput] = outputs[sourceOutput].size();
                } else {
                    std::size_t packIndex = kNoIndex;
                    for (std::size_t i = 0; i < folder.packInputIndices.size(); ++i)
                        if (folder.packInputIndices[i] == inputIndex) packIndex = i;
                    if (packIndex == kNoIndex || packIndex >= packed.size()) return false;
                    const PackedStream& source = packed[packIndex];
                    inputData[localInput] = source.data;
                    inputSizes[localInput] = source.size;
                }
            }
            if (!ready) continue;
            std::vector<std::uint8_t> decoded;
            const std::uint64_t expectedSize = folder.outputSizes[coder.outputBase];
            if (coder.method == QByteArray::fromHex("0303011b")) {
                if (coder.inputCount != 4 || coder.outputCount != 1 ||
                    !coder.properties.isEmpty()) return false;
                std::array<const std::uint8_t*, 4> branches = {};
                std::array<std::size_t, 4> branchSizes = {};
                for (std::size_t i = 0; i < 4; ++i) {
                    branches[i] = inputData[i];
                    branchSizes[i] = inputSizes[i];
                }
                if (!compression::decodeBcj2(branches, branchSizes,
                    std::size_t(expectedSize), &decoded)) return false;
            } else if (coder.method == QByteArray::fromHex("06f10701") &&
                       coder.inputCount == 1 && coder.outputCount == 1) {
                if (password.isNull()) {
                    if (openError) *openError = ModuleOpenError::PasswordRequired;
                    return false;
                }
                if (!compression::decryptSevenZipAes(
                    coder.properties, password, inputData[0], inputSizes[0],
                    std::size_t(expectedSize), &decoded)) {
                    if (openError) *openError = ModuleOpenError::PasswordInvalidOrCorrupt;
                    return false;
                }
            } else if (coder.inputCount == 1 && coder.outputCount == 1 &&
                       isFilterMethod(coder.method)) {
                if (inputSizes[0] > kMaximumDecodedSize) return false;
                if (inputSizes[0])
                    decoded.assign(inputData[0], inputData[0] + inputSizes[0]);
                if (decoded.size() != expectedSize ||
                    !applyFilter(coder.method, coder.properties, &decoded)) return false;
            } else if (coder.inputCount == 1 && coder.outputCount == 1 &&
                       isCodecMethod(coder.method)) {
                if (!decodeCodec(coder.method, coder.properties, expectedSize,
                                 inputData[0], inputSizes[0], &decoded)) return false;
            } else {
                return false;
            }
            if (decoded.size() != expectedSize) return false;
            for (std::size_t localOutput = 0; localOutput < coder.outputCount; ++localOutput) {
                const std::size_t outputIndex = coder.outputBase + localOutput;
                if (outputIndex != coder.outputBase || outputReady[outputIndex]) return false;
                outputs[outputIndex] = std::move(decoded);
                outputReady[outputIndex] = true;
            }
            coderDone[coderIndex] = true;
            ++completedCoders;
            progressed = true;
        }
        if (!progressed) return false;
    }
    if (folder.finalOutputIndex >= outputs.size() || !outputReady[folder.finalOutputIndex] ||
        outputs[folder.finalOutputIndex].size() != folder.unpackSize) return false;
    output->swap(outputs[folder.finalOutputIndex]);
    return true;
}

class SevenZipDeflateStore final : public fs::IByteStore {
public:
    SevenZipDeflateStore(std::shared_ptr<void> archiveOwner,
                         const std::uint8_t* archiveData,
                         std::size_t packedOffset, std::size_t packedSize,
                         std::size_t unpackedSize, Digest packedDigest,
                         Digest folderDigest, Digest fileDigest)
        : archiveOwner_(std::move(archiveOwner)), archiveData_(archiveData),
          packedOffset_(packedOffset),
          packedSize_(packedSize), unpackedSize_(unpackedSize),
          packedDigest_(packedDigest), folderDigest_(folderDigest),
          fileDigest_(fileDigest) {}

    std::int64_t capacity() const override {
        return static_cast<std::int64_t>(unpackedSize_);
    }

    bool cheapRandomAccess() const override { return false; }

    bool validate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return ensureFull();
    }

    bool copyTo(std::uint8_t* destination, std::size_t length) const override {
        if (length != unpackedSize_ || (length && !destination)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (fullReady_) {
            std::memcpy(destination, decoded_.data(), length);
            return true;
        }
        if (fullFailed_ || !archiveOwner_ || !archiveData_) return false;
        const std::uint8_t* packed = packedData();
        std::future<std::uint32_t> packedCrc;
        bool verifyPackedAfterDecode = false;
        if (packedDigest_.defined) {
            if (packedSize_ >= 1024 * 1024) {
                try {
                    const std::size_t packedSize = packedSize_;
                    packedCrc = std::async(std::launch::async,
                        [packed, packedSize]() {
                            return crc32(packed, packedSize);
                        });
                    verifyPackedAfterDecode = true;
                } catch (...) {
                    if (crc32(packed, packedSize_) != packedDigest_.value) {
                        fullFailed_ = true;
                        archiveOwner_.reset();
                        archiveData_ = nullptr;
                        return false;
                    }
                }
            } else if (crc32(packed, packedSize_) != packedDigest_.value) {
                fullFailed_ = true;
                archiveOwner_.reset();
                archiveData_ = nullptr;
                return false;
            }
        }
        std::string error;
        const bool decoded = compression::inflateRawExactToBuffer(
            packed, packedSize_, unpackedSize_, destination, &error);
        const bool packedValid = !verifyPackedAfterDecode ||
            packedCrc.get() == packedDigest_.value;
        if (!decoded || !packedValid) {
            fullFailed_ = true;
            archiveOwner_.reset();
            archiveData_ = nullptr;
            return false;
        }
        if (folderDigest_.defined && crc32Parallel(destination, length) != folderDigest_.value) {
            fullFailed_ = true;
            archiveOwner_.reset();
            archiveData_ = nullptr;
            return false;
        }
        const bool fileDigestCovered = folderDigest_.defined && fileDigest_.defined &&
                                       folderDigest_.value == fileDigest_.value;
        if (fileDigest_.defined && !fileDigestCovered &&
            crc32Parallel(destination, length) != fileDigest_.value) {
            fullFailed_ = true;
            archiveOwner_.reset();
            archiveData_ = nullptr;
            return false;
        }
        return true;
    }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!dst || pos < 0 || count <= 0 ||
            pos >= static_cast<std::int64_t>(unpackedSize_)) return 0;
        const std::size_t offset = static_cast<std::size_t>(pos);
        const std::size_t length = std::min<std::size_t>(
            static_cast<std::size_t>(count), unpackedSize_ - offset);
        const std::size_t end = offset + length;
        std::lock_guard<std::mutex> lock(mutex_);
        if (fullReady_) {
            std::memcpy(dst, decoded_.data() + offset, length);
            return static_cast<int>(length);
        }
        if (end <= kPrefixLimit && end < unpackedSize_) {
            if (!ensurePrefix(end)) return 0;
            std::memcpy(dst, prefix_.data() + offset, length);
            return static_cast<int>(length);
        }
        if (!ensureFull()) return 0;
        std::memcpy(dst, decoded_.data() + offset, length);
        return static_cast<int>(length);
    }

private:
    static const std::size_t kPrefixLimit = 16 * 1024 * 1024;

    const std::uint8_t* packedData() const {
        return archiveData_ + packedOffset_;
    }

    bool ensurePrefix(std::size_t wanted) const {
        if (prefix_.size() >= wanted) return true;
        if (!archiveOwner_ || !archiveData_ || wanted > kPrefixLimit ||
            wanted >= unpackedSize_)
            return ensureFull();
        std::vector<std::uint8_t> output;
        if (!compression::inflateRawPrefix(packedData(), packedSize_, wanted, &output) ||
            output.size() != wanted) return false;
        prefix_.swap(output);
        return true;
    }

    bool ensureFull() const {
        if (fullReady_) return true;
        if (fullFailed_ || !archiveOwner_ || !archiveData_) return false;
        const std::uint8_t* packed = packedData();
        std::future<std::uint32_t> packedCrc;
        bool verifyPackedAfterDecode = false;
        if (packedDigest_.defined) {
            if (packedSize_ >= 1024 * 1024) {
                try {
                    const std::size_t packedSize = packedSize_;
                    packedCrc = std::async(std::launch::async,
                        [packed, packedSize]() {
                            return crc32(packed, packedSize);
                        });
                    verifyPackedAfterDecode = true;
                } catch (...) {
                    if (crc32(packed, packedSize_) != packedDigest_.value) {
                        fullFailed_ = true;
                        archiveOwner_.reset();
                        archiveData_ = nullptr;
                        return false;
                    }
                }
            } else if (crc32(packed, packedSize_) != packedDigest_.value) {
                fullFailed_ = true;
                archiveOwner_.reset();
                archiveData_ = nullptr;
                return false;
            }
        }
        std::vector<std::uint8_t> output;
        std::string error;
        const bool decoded = compression::inflateRawExact(
            packed, packedSize_, unpackedSize_, nullptr, &output, &error);
        const bool packedValid = !verifyPackedAfterDecode ||
            packedCrc.get() == packedDigest_.value;
        if (!decoded || output.size() != unpackedSize_ || !packedValid ||
            (folderDigest_.defined && crc32Parallel(output.data(), output.size()) !=
                                      folderDigest_.value)) {
            fullFailed_ = true;
            archiveOwner_.reset();
            archiveData_ = nullptr;
            return false;
        }
        const bool fileDigestCovered = folderDigest_.defined && fileDigest_.defined &&
                                       folderDigest_.value == fileDigest_.value;
        if (fileDigest_.defined && !fileDigestCovered &&
            crc32Parallel(output.data(), output.size()) != fileDigest_.value) {
            fullFailed_ = true;
            archiveOwner_.reset();
            archiveData_ = nullptr;
            return false;
        }
        decoded_.swap(output);
        prefix_.clear();
        fullReady_ = true;
        archiveOwner_.reset();
        archiveData_ = nullptr;
        return true;
    }

    mutable std::shared_ptr<void> archiveOwner_;
    mutable const std::uint8_t* archiveData_;
    std::size_t packedOffset_;
    std::size_t packedSize_;
    std::size_t unpackedSize_;
    Digest packedDigest_;
    Digest folderDigest_;
    Digest fileDigest_;
    mutable std::mutex mutex_;
    mutable std::vector<std::uint8_t> prefix_;
    mutable std::vector<std::uint8_t> decoded_;
    mutable bool fullReady_ = false;
    mutable bool fullFailed_ = false;
};

bool decodeAdditionalStreams(const std::uint8_t* bytes, std::size_t archiveSize,
                             const StreamsInfo& streams,
                             const QString& password,
                             std::vector<std::vector<std::uint8_t>>* substreams)
{
    if (!substreams || !bytes || archiveSize < kStartHeaderSize ||
        streams.pack.position > std::uint64_t(archiveSize - kStartHeaderSize)) return false;
    std::size_t packOffset = kStartHeaderSize + std::size_t(streams.pack.position);
    std::size_t packIndex = 0;
    substreams->clear();
    for (std::size_t folderIndex = 0; folderIndex < streams.folders.size(); ++folderIndex) {
        const Folder& folder = streams.folders[folderIndex];
        std::vector<PackedStream> packed;
        packed.reserve(folder.packStreamCount);
        for (std::size_t i = 0; i < folder.packStreamCount; ++i) {
            if (packIndex >= streams.pack.sizes.size()) return false;
            const std::uint64_t size = streams.pack.sizes[packIndex];
            if (packOffset > archiveSize ||
                size > archiveSize - packOffset ||
                size > std::numeric_limits<std::size_t>::max()) return false;
            if (!streams.pack.digests.empty() && streams.pack.digests[packIndex].defined &&
                crc32(bytes + packOffset, std::size_t(size)) !=
                    streams.pack.digests[packIndex].value) return false;
            packed.push_back({bytes + packOffset, std::size_t(size)});
            packOffset += std::size_t(size);
            ++packIndex;
        }
        std::vector<std::uint8_t> decoded;
        if (!decodeFolder(folder, packed, password, &decoded, nullptr) ||
            decoded.size() != folder.unpackSize ||
            (folder.digest.defined && crc32Parallel(decoded.data(), decoded.size()) != folder.digest.value))
            return false;
        std::size_t offset = 0;
        for (std::uint64_t i = 0; i < streams.substreamCounts[folderIndex]; ++i) {
            const std::uint64_t size64 = streams.substreamSizes[substreams->size()];
            if (size64 > decoded.size() - offset) return false;
            const std::size_t size = std::size_t(size64);
            const Digest digest = streams.substreamDigests[substreams->size()];
            const std::uint8_t* payload = size ? decoded.data() + offset : nullptr;
            if (digest.defined && crc32Parallel(payload, size) != digest.value) return false;
            if (size)
                substreams->emplace_back(payload, payload + size);
            else
                substreams->emplace_back();
            offset += size;
        }
        if (offset != decoded.size()) return false;
    }
    return packIndex == streams.pack.sizes.size();
}

bool decodeEncodedHeader(const std::uint8_t* archive, std::size_t archiveSize,
                         Cursor& encoded, const QString& password,
                         std::vector<std::uint8_t>* output,
                         ModuleOpenError* openError)
{
    StreamsInfo streams;
    if (!output || !parseStreamsInfo(encoded, &streams)) return false;
    if (encoded.remaining() ||
        streams.folders.size() != 1 ||
        streams.pack.sizes.size() != streams.folders[0].packStreamCount ||
        streams.substreamCounts.size() != 1 || streams.substreamCounts[0] != 1 ||
        streams.substreamSizes.size() != 1 || streams.substreamSizes[0] != streams.folders[0].unpackSize ||
        streams.pack.position > archiveSize - kStartHeaderSize) return false;
    std::size_t packedOffset = kStartHeaderSize + std::size_t(streams.pack.position);
    std::vector<PackedStream> packedStreams;
    packedStreams.reserve(streams.pack.sizes.size());
    for (std::size_t i = 0; i < streams.pack.sizes.size(); ++i) {
        const std::uint64_t packedSize = streams.pack.sizes[i];
        if (packedSize > archiveSize - packedOffset ||
            packedSize > std::numeric_limits<std::size_t>::max()) return false;
        if (!streams.pack.digests.empty() && streams.pack.digests[i].defined &&
            crc32(archive + packedOffset, std::size_t(packedSize)) !=
                streams.pack.digests[i].value) return false;
        packedStreams.push_back({archive + packedOffset, std::size_t(packedSize)});
        packedOffset += std::size_t(packedSize);
    }
    std::vector<std::uint8_t> decoded;
    if (!decodeFolder(streams.folders[0], packedStreams, password, &decoded, openError) ||
        decoded.size() != streams.folders[0].unpackSize) {
        if (openError && !password.isNull() && *openError == ModuleOpenError::None)
            *openError = ModuleOpenError::PasswordInvalidOrCorrupt;
        return false;
    }
    const Digest digest = streams.substreamDigests.empty()
        ? streams.folders[0].digest : streams.substreamDigests[0];
    if (digest.defined && crc32Parallel(decoded.data(), decoded.size()) != digest.value) {
        if (openError && !password.isNull())
            *openError = ModuleOpenError::PasswordInvalidOrCorrupt;
        return false;
    }
    output->swap(decoded);
    return true;
}

bool openArchive(const std::uint8_t* bytes, std::size_t archiveSize,
                 const QString& logicalName,
                 const QString& password, QVector<ResourceEntry>* resources,
                 ModuleOpenError* openError,
                 const std::shared_ptr<void>& archiveOwner)
{
    if (!resources || !bytes || archiveSize < kStartHeaderSize ||
        archiveSize > kMaximumArchiveSize ||
        std::memcmp(bytes, kSignature, sizeof(kSignature)) != 0)
        return false;
    if (bytes[6] != 0 || crc32(bytes + 12, 20) != readLe32(bytes + 8)) {
        return false;
    }
    const std::uint64_t nextOffset = qFromLittleEndian<std::uint64_t>(bytes + 12);
    const std::uint64_t nextSize = qFromLittleEndian<std::uint64_t>(bytes + 20);
    if (nextOffset > std::uint64_t(archiveSize - kStartHeaderSize) ||
        nextSize > std::uint64_t(archiveSize - kStartHeaderSize) - nextOffset ||
        nextSize > std::numeric_limits<std::size_t>::max()) return false;
    const auto* next = bytes + kStartHeaderSize + std::size_t(nextOffset);
    const std::size_t nextLength = std::size_t(nextSize);
    if (crc32(next, nextLength) != readLe32(bytes + 28)) {
        return false;
    }
    Cursor header(next, nextLength);
    std::uint8_t property = 0;
    if (!header.byte(&property)) return false;
    std::vector<std::uint8_t> decodedHeader;
    if (property == 0x17) {
        if (!decodeEncodedHeader(bytes, archiveSize, header, password,
                                 &decodedHeader, openError))
            return false;
        header = Cursor(decodedHeader.data(), decodedHeader.size());
        if (!header.byte(&property)) return false;
    }
    if (property != 1) {
        return false;
    }
    StreamsInfo streams;
    StreamsInfo additionalStreams;
    std::vector<std::vector<std::uint8_t>> additionalSubstreams;
    bool haveStreams = false;
    bool haveAdditionalStreams = false;
    std::vector<FileInfo> files;
    for (;;) {
        if (!header.byte(&property)) return false;
        if (property == 0) break;
        if (property == 2) {
            if (!skipProperties(header)) return false;
        } else if (property == 3) {
            if (haveAdditionalStreams ||
                !parseStreamsInfo(header, &additionalStreams) ||
                !decodeAdditionalStreams(bytes, archiveSize, additionalStreams, password,
                                         &additionalSubstreams)) return false;
            haveAdditionalStreams = true;
        } else if (property == 4) {
            if (haveStreams || !parseStreamsInfo(header, &streams,
                                                  &additionalSubstreams)) {
                return false;
            }
            haveStreams = true;
        } else if (property == 5) {
            if (!parseFilesInfo(header, &files)) {
                return false;
            }
        } else {
            return false;
        }
    }
    if (header.remaining() ||
        streams.substreamCounts.size() != streams.folders.size() ||
        streams.substreamSizes.size() != streams.substreamDigests.size()) return false;

    std::size_t fileStreamCount = 0;
    for (const FileInfo& file : files) if (!file.emptyStream) ++fileStreamCount;
    if (fileStreamCount != streams.substreamSizes.size()) return false;
    const bool hasExternalNames = std::any_of(files.begin(), files.end(),
        [](const FileInfo& file) { return file.externalName; });
    if (hasExternalNames) {
        if (!haveAdditionalStreams) return false;
        const std::uint64_t nameStreamIndex = std::find_if(files.begin(), files.end(),
            [](const FileInfo& file) { return file.externalName; })->nameStreamIndex;
        if (nameStreamIndex >= std::uint64_t(additionalSubstreams.size())) return false;
        const auto& namesData = additionalSubstreams[std::size_t(nameStreamIndex)];
        Cursor names(namesData.data(), namesData.size());
        for (FileInfo& file : files) {
            if (!file.externalName) continue;
            if (file.nameStreamIndex != nameStreamIndex) return false;
            QVector<ushort> name;
            for (;;) {
                const std::uint8_t* unit = nullptr;
                if (!names.bytes(2, &unit)) return false;
                const ushort value = qFromLittleEndian<ushort>(unit);
                if (!value) break;
                name.push_back(value);
            }
            file.name = QString::fromUtf16(name.constData(), name.size());
        }
        if (names.remaining()) return false;
    }
    if (streams.pack.position > std::uint64_t(archiveSize - kStartHeaderSize))
        return false;
    const std::size_t packedStart = kStartHeaderSize + std::size_t(streams.pack.position);
    std::size_t packOffset = packedStart;
    std::vector<std::size_t> streamResources;
    streamResources.reserve(fileStreamCount);
    resources->reserve(files.size());
    for (std::size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
        const FileInfo& file = files[fileIndex];
        QString name = file.name.isEmpty()
            ? QStringLiteral("file_%1").arg(fileIndex) : file.name;
        QStringList parts = safePath(name);
        if (parts.isEmpty()) return false;
        ResourceEntry entry;
        entry.type = QStringLiteral("7Z_FILE");
        entry.name = parts.takeLast();
        entry.language = QStringLiteral("neutral");
        entry.format = ModuleFormat::SEVEN_ZIP;
        entry.hierarchyPath = parts;
        if (file.emptyStream) {
            entry.type = file.emptyFile ? QStringLiteral("7Z_FILE") : QStringLiteral("7Z_DIR");
            entry.isDirectory = !file.emptyFile;
            entry.isEmbeddedFile = file.emptyFile;
            entry.dataSize = 0;
        } else {
            entry.isEmbeddedFile = true;
            streamResources.push_back(resources->size());
        }
        resources->push_back(std::move(entry));
    }
    if (streamResources.size() != streams.substreamSizes.size()) return false;

    const auto* archiveData = bytes;
    std::size_t streamIndex = 0;
    std::size_t packSizeIndex = 0;
    for (std::size_t folderIndex = 0; folderIndex < streams.folders.size(); ++folderIndex) {
        const Folder& folder = streams.folders[folderIndex];
        if (folder.unpackSize > kMaximumDecodedSize) return false;
        const bool lazyDeflate = streams.substreamCounts[folderIndex] == 1 &&
            folder.packStreamCount == 1 && folder.coders.size() == 1 &&
            folder.coders[0].method == QByteArray::fromHex("040108") &&
            folder.coders[0].properties.isEmpty() &&
            folder.coders[0].inputCount == 1 && folder.coders[0].outputCount == 1;
        const std::size_t folderPackedOffset = packOffset;
        std::vector<PackedStream> packedStreams;
        packedStreams.reserve(folder.packStreamCount);
        Digest packedDigest;
        for (std::size_t i = 0; i < folder.packStreamCount; ++i) {
            if (packSizeIndex >= streams.pack.sizes.size()) return false;
            const std::uint64_t packedSize = streams.pack.sizes[packSizeIndex];
            if (packOffset > archiveSize ||
                packedSize > archiveSize - packOffset ||
                packedSize > std::numeric_limits<std::size_t>::max()) return false;
            const Digest digest = streams.pack.digests.empty()
                ? Digest() : streams.pack.digests[packSizeIndex];
            if (i == 0) packedDigest = digest;
            if (!lazyDeflate && digest.defined &&
                crc32(bytes + packOffset, std::size_t(packedSize)) != digest.value)
                return false;
            packedStreams.push_back({bytes + packOffset, std::size_t(packedSize)});
            packOffset += std::size_t(packedSize);
            ++packSizeIndex;
        }
        if (lazyDeflate) {
            if (streamIndex >= streamResources.size() ||
                streams.substreamSizes[streamIndex] != folder.unpackSize)
                return false;
            ResourceEntry& entry = (*resources)[streamResources[streamIndex]];
            entry.content = std::make_shared<SevenZipDeflateStore>(
                archiveOwner, archiveData, folderPackedOffset, packedStreams[0].size,
                std::size_t(folder.unpackSize), packedDigest, folder.digest,
                streams.substreamDigests[streamIndex]);
            entry.dataSize = folder.unpackSize;
            ++streamIndex;
            continue;
        }
        std::vector<std::uint8_t> output;
        const bool decodedFolder = decodeFolder(folder, packedStreams, password, &output,
                                                openError);
        const bool encryptedFolder = std::any_of(folder.coders.begin(), folder.coders.end(),
            [](const CoderDescription& coder) {
                return coder.method == QByteArray::fromHex("06f10701");
            });
        if (!decodedFolder || output.size() != folder.unpackSize ||
            (folder.digest.defined && crc32Parallel(output.data(), output.size()) != folder.digest.value)) {
            if (encryptedFolder && openError && !password.isNull() &&
                *openError == ModuleOpenError::None)
                *openError = ModuleOpenError::PasswordInvalidOrCorrupt;
            return false;
        }
        const bool oversizedSubstream = std::any_of(
            streams.substreamSizes.begin() + std::ptrdiff_t(streamIndex),
            streams.substreamSizes.begin() + std::ptrdiff_t(
                streamIndex + std::size_t(streams.substreamCounts[folderIndex])),
            [](std::uint64_t size) {
                return size > std::uint64_t(std::numeric_limits<int>::max());
            });
        std::shared_ptr<fs::MemoryStore> folderStore;
        if (oversizedSubstream)
            folderStore = std::make_shared<fs::MemoryStore>(std::move(output));
        const std::uint8_t* folderData = folderStore
            ? folderStore->bytes().data() : output.data();
        const std::size_t folderSize = folderStore
            ? folderStore->bytes().size() : output.size();
        std::size_t outputOffset = 0;
        for (std::uint64_t substream = 0; substream < streams.substreamCounts[folderIndex];
             ++substream) {
            if (streamIndex >= streamResources.size()) return false;
            const std::uint64_t substreamSize64 = streams.substreamSizes[streamIndex];
            if (substreamSize64 > output.size() - outputOffset) return false;
            const std::size_t substreamSize = std::size_t(substreamSize64);
            const std::uint8_t* payload = folderSize ? folderData + outputOffset : nullptr;
            const Digest digest = streams.substreamDigests[streamIndex];
            const bool folderDigestCoversFile =
                streams.substreamCounts[folderIndex] == 1 && folder.digest.defined &&
                digest.defined && folder.digest.value == digest.value;
            if (digest.defined && !folderDigestCoversFile &&
                crc32Parallel(payload, substreamSize) != digest.value) {
                if (encryptedFolder && openError && !password.isNull())
                    *openError = ModuleOpenError::PasswordInvalidOrCorrupt;
                return false;
            }
            ResourceEntry& entry = (*resources)[streamResources[streamIndex]];
            if (substreamSize > std::size_t(std::numeric_limits<int>::max())) {
                if (!folderStore || outputOffset > std::size_t(
                        std::numeric_limits<std::int64_t>::max()) ||
                    substreamSize > std::size_t(
                        std::numeric_limits<std::int64_t>::max())) return false;
                entry.content = std::make_shared<fs::SubStore>(
                    folderStore, std::int64_t(outputOffset), std::int64_t(substreamSize));
            } else if (substreamSize) {
                entry.data = QByteArray(reinterpret_cast<const char*>(payload), int(substreamSize));
            }
            entry.dataSize = substreamSize;
            outputOffset += substreamSize;
            ++streamIndex;
        }
        if (outputOffset != folderSize) return false;
    }
    if (streamIndex != streamResources.size() || packSizeIndex != streams.pack.sizes.size())
        return false;
    Q_UNUSED(logicalName);
    return true;
}

}  // namespace

ModulePtr SevenZipModule::open(const QString& filePath, const QString& password)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<SevenZipModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::SEVEN_ZIP;
        module->info_.description = QStringLiteral("7-Zip archive");
        module->info_.error = file.errorString();
        return module;
    }
    if (filePath.endsWith(QStringLiteral(".001"), Qt::CaseInsensitive)) {
        auto data = std::make_shared<std::vector<std::uint8_t>>();
        const auto appendVolume = [](QFile& volume, std::vector<std::uint8_t>* output) {
            std::array<std::uint8_t, 1024 * 1024> chunk{};
            for (;;) {
                const qint64 count = volume.read(
                    reinterpret_cast<char*>(chunk.data()), qint64(chunk.size()));
                if (count < 0) return false;
                if (!count) return volume.atEnd();
                const std::size_t length = std::size_t(count);
                if (length > output->max_size() - output->size()) return false;
                output->insert(output->end(), chunk.begin(), chunk.begin() + count);
            }
        };
        if (!appendVolume(file, data.get())) data->clear();
        if (data->size() >= kStartHeaderSize &&
            std::memcmp(data->data(), kSignature, sizeof(kSignature)) == 0) {
            const auto* header = data->data();
            const std::uint64_t nextOffset = qFromLittleEndian<std::uint64_t>(header + 12);
            const std::uint64_t nextSize = qFromLittleEndian<std::uint64_t>(header + 20);
            if (nextOffset <= kMaximumArchiveSize - kStartHeaderSize &&
            nextSize <= kMaximumArchiveSize - kStartHeaderSize - nextOffset) {
                const std::uint64_t requiredSize = kStartHeaderSize + nextOffset + nextSize;
                for (unsigned volume = 2; std::uint64_t(data->size()) < requiredSize &&
                     volume <= 10000; ++volume) {
                    const QString volumePath = filePath.left(filePath.size() - 3) +
                        QString::number(volume).rightJustified(3, QLatin1Char('0'));
                    QFile nextVolume(volumePath);
                    if (!nextVolume.open(QIODevice::ReadOnly) ||
                        !appendVolume(nextVolume, data.get())) break;
                }
            }
        }
        auto module = peare::makeUnique<SevenZipModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::SEVEN_ZIP;
        module->info_.description = QStringLiteral("7-Zip archive");
        std::shared_ptr<void> owner = data;
        if (!openArchive(data->data(), data->size(), filePath, password,
                         &module->resources_, &module->info_.openError, owner)) {
            module->resources_.clear();
            module->info_.error = QStringLiteral("Unsupported, invalid, or corrupt 7-Zip archive");
        }
        return ModulePtr(std::move(module));
    }

    file.close();
    const auto mapped = std::make_shared<MappedArchive>(filePath);
    if (!mapped->isValid()) {
        auto source = std::make_shared<fs::QFileByteStore>(filePath);
        if (source->validate()) return open(source, filePath, password);
        auto module = peare::makeUnique<SevenZipModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::SEVEN_ZIP;
        module->info_.description = QStringLiteral("7-Zip archive");
        module->info_.error = mapped->error();
        return module;
    }
    auto module = peare::makeUnique<SevenZipModule>();
    module->info_.filePath = filePath;
    module->info_.format = ModuleFormat::SEVEN_ZIP;
    module->info_.description = QStringLiteral("7-Zip archive");
    std::shared_ptr<void> owner = mapped;
    if (!openArchive(reinterpret_cast<const std::uint8_t*>(mapped->data()),
                     std::size_t(mapped->size()), filePath, password, &module->resources_,
                     &module->info_.openError, owner)) {
        module->resources_.clear();
        module->info_.error = QStringLiteral("Unsupported, invalid, or corrupt 7-Zip archive");
    }
    return ModulePtr(std::move(module));
}

ModulePtr SevenZipModule::open(const fs::ByteStorePtr& source,
                               const QString& logicalName,
                               const QString& password)
{
    auto module = peare::makeUnique<SevenZipModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::SEVEN_ZIP;
    module->info_.description = QStringLiteral("7-Zip archive");
    if (!source || source->capacity() < 0 ||
        std::uint64_t(source->capacity()) > std::numeric_limits<std::size_t>::max()) {
        module->info_.error = QStringLiteral("Invalid 7-Zip byte source");
        return ModulePtr(std::move(module));
    }
    auto archive = std::make_shared<std::vector<std::uint8_t>>(source->readAll());
    if (std::uint64_t(archive->size()) != std::uint64_t(source->capacity())) {
        module->info_.error = QStringLiteral("Short read from 7-Zip byte source");
        return ModulePtr(std::move(module));
    }
    std::shared_ptr<void> owner = archive;
    if (!openArchive(archive->data(), archive->size(), logicalName, password,
                     &module->resources_, &module->info_.openError, owner)) {
        module->resources_.clear();
        module->info_.error = QStringLiteral("Unsupported, invalid, or corrupt 7-Zip archive");
    }
    return ModulePtr(std::move(module));
}

ModulePtr SevenZipModule::open(const QByteArray& data, const QString& logicalName,
                               const QString& password)
{
    auto module = peare::makeUnique<SevenZipModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::SEVEN_ZIP;
    module->info_.description = QStringLiteral("7-Zip archive");
    auto archiveOwner = std::make_shared<QByteArray>(data.constData(), data.size());
    std::shared_ptr<void> owner = archiveOwner;
    if (!openArchive(reinterpret_cast<const std::uint8_t*>(archiveOwner->constData()),
                     std::size_t(archiveOwner->size()), logicalName, password,
                     &module->resources_,
                     &module->info_.openError, owner)) {
        module->resources_.clear();
        module->info_.error = QStringLiteral("Unsupported, invalid, or corrupt 7-Zip archive");
    }
    return ModulePtr(std::move(module));
}

}  // namespace peare
