#include "LzipModule.h"
#include "Compat.h"

#include "../compression/Crc32.h"
#include "../compression/LzmaDecoder.h"
#include "../fs/DiscStore.h"
#include "../fs/QFileByteStore.h"

#include <QFileInfo>

#include <limits>
#include <utility>

namespace peare {
namespace {

constexpr std::size_t kHeaderSize = 6;
constexpr std::size_t kTrailerSize = 20;
constexpr std::uint32_t kMinimumDictionarySize = 4096;
constexpr std::uint32_t kMaximumDictionarySize = 512u * 1024u * 1024u;

std::uint32_t readLe32(const std::uint8_t* data)
{
    return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) |
        (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
}

std::uint64_t readLe64(const std::uint8_t* data)
{
    return std::uint64_t(readLe32(data)) |
        (std::uint64_t(readLe32(data + 4)) << 32);
}

bool dictionarySize(std::uint8_t encoded, std::uint32_t* size)
{
    const unsigned exponent = encoded & 0x1Fu;
    if (exponent < 12 || exponent > 29) return false;
    const std::uint32_t base = std::uint32_t(1) << exponent;
    const std::uint32_t result = base - (base >> 4) * (encoded >> 5);
    if (result < kMinimumDictionarySize || result > kMaximumDictionarySize) return false;
    *size = result;
    return true;
}

bool isMemberHeader(const std::vector<std::uint8_t>& data, std::size_t offset,
                    std::uint32_t* dictionary)
{
    if (offset > data.size() || data.size() - offset < kHeaderSize ||
        data[offset] != 'L' || data[offset + 1] != 'Z' ||
        data[offset + 2] != 'I' || data[offset + 3] != 'P' ||
        data[offset + 4] != 1)
        return false;
    return dictionarySize(data[offset + 5], dictionary);
}

bool decodeMember(const std::vector<std::uint8_t>& data, std::size_t start,
                  std::size_t end, std::uint32_t dictionary,
                  std::vector<std::uint8_t>* output)
{
    if (end < start || end - start < kHeaderSize + kTrailerSize ||
        end > data.size())
        return false;
    const std::size_t trailerOffset = end - kTrailerSize;
    if (trailerOffset < start + kHeaderSize) return false;
    const std::uint8_t* trailer = data.data() + trailerOffset;
    const std::uint64_t unpackedSize64 = readLe64(trailer + 4);
    if (unpackedSize64 > output->max_size()) return false;

    const std::uint8_t properties[5] = {
        0x5D,
        static_cast<std::uint8_t>(dictionary),
        static_cast<std::uint8_t>(dictionary >> 8),
        static_cast<std::uint8_t>(dictionary >> 16),
        static_cast<std::uint8_t>(dictionary >> 24)
    };
    std::vector<std::uint8_t> decoded;
    const std::size_t packedOffset = start + kHeaderSize;
    const std::size_t packedSize = trailerOffset - packedOffset;
    if (!compression::decodeLzmaRawWithEndMarker(
            data.data() + packedOffset, packedSize, properties, sizeof(properties),
            static_cast<std::size_t>(unpackedSize64), &decoded) ||
        compression::crc32(decoded.data(), decoded.size()) != readLe32(trailer) ||
        decoded.size() != unpackedSize64 || readLe64(trailer + 12) != end - start)
        return false;
    *output = std::move(decoded);
    return true;
}

QString payloadName(const QString& logicalName)
{
    QString name = QFileInfo(logicalName).fileName();
    if (name.endsWith(QStringLiteral(".tlz"), Qt::CaseInsensitive)) name.chop(4);
    else if (name.endsWith(QStringLiteral(".lz"), Qt::CaseInsensitive)) name.chop(3);
    return name.isEmpty() ? QStringLiteral("lzip_payload") : name;
}

} // namespace

std::unique_ptr<LzipModule> LzipModule::open(const QString& filePath)
{
    auto source = std::make_shared<fs::QFileByteStore>(filePath);
    if (!source->validate()) {
        auto module = peare::makeUnique<LzipModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::LZIP;
        module->info_.error = QStringLiteral("Could not read Lzip input");
        return module;
    }
    return open(source, filePath);
}

std::unique_ptr<LzipModule> LzipModule::open(const fs::ByteStorePtr& source,
                                              const QString& logicalName)
{
    if (!source || source->capacity() < 0 ||
        std::uint64_t(source->capacity()) > std::numeric_limits<std::size_t>::max()) {
        auto module = peare::makeUnique<LzipModule>();
        module->info_.filePath = logicalName;
        module->info_.format = ModuleFormat::LZIP;
        module->info_.error = QStringLiteral("Invalid Lzip byte source");
        return module;
    }
    return open(source->readAll(), logicalName);
}

std::unique_ptr<LzipModule> LzipModule::open(const QByteArray& data,
                                              const QString& logicalName)
{
    return open(std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(data.constData()),
        reinterpret_cast<const std::uint8_t*>(data.constData()) + data.size()), logicalName);
}

std::unique_ptr<LzipModule> LzipModule::open(std::vector<std::uint8_t> data,
                                              const QString& logicalName)
{
    auto module = peare::makeUnique<LzipModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::LZIP;
    module->info_.description = QStringLiteral("Lzip compressed stream");

    std::vector<std::vector<std::uint8_t>> members;
    std::size_t end = data.size();
    bool hasEmptyMember = false;
    const auto acceptMember = [&](std::size_t memberEnd) {
        if (memberEnd < kHeaderSize + kTrailerSize || memberEnd > end) return false;
        const std::uint64_t memberSize = readLe64(data.data() + memberEnd - 8);
        if (memberSize < kHeaderSize + kTrailerSize || memberSize > memberEnd) return false;
        const std::size_t start = memberEnd - static_cast<std::size_t>(memberSize);
        std::uint32_t dictionary = 0;
        if (!isMemberHeader(data, start, &dictionary)) return false;
        std::vector<std::uint8_t> member;
        if (!decodeMember(data, start, memberEnd, dictionary, &member)) return false;
        hasEmptyMember = hasEmptyMember || member.empty();
        members.push_back(std::move(member));
        end = start;
        return true;
    };

    if (end >= kHeaderSize + kTrailerSize && !acceptMember(end)) {
        for (std::size_t candidateEnd = end - 1;
             candidateEnd >= kHeaderSize + kTrailerSize; --candidateEnd) {
            if (!acceptMember(candidateEnd)) {
                if (candidateEnd == kHeaderSize + kTrailerSize) break;
                continue;
            }
            if (candidateEnd < data.size() && data.size() - candidateEnd >= 4 &&
                data[candidateEnd] == 'L' && data[candidateEnd + 1] == 'Z' &&
                data[candidateEnd + 2] == 'I' && data[candidateEnd + 3] == 'P') {
                module->info_.error = QStringLiteral("Invalid trailing Lzip member");
                return module;
            }
            break;
        }
    }

    if (members.empty()) {
        module->info_.error = QStringLiteral("Lzip stream has no valid members");
        return module;
    }
    while (end > 0 && acceptMember(end)) {
    }
    if (end != 0 || (members.size() > 1 && hasEmptyMember)) {
        module->info_.error = QStringLiteral("Lzip stream has no valid members");
        return module;
    }

    std::vector<std::uint8_t> decoded;
    for (auto member = members.rbegin(); member != members.rend(); ++member) {
        if (member->size() > decoded.max_size() - decoded.size()) {
            module->info_.error = QStringLiteral("Lzip output is too large");
            return module;
        }
        decoded.insert(decoded.end(), member->begin(), member->end());
    }

    ResourceEntry entry;
    entry.type = QStringLiteral("LZIP_FILE");
    entry.name = payloadName(logicalName);
    entry.language = QStringLiteral("neutral");
    entry.dataSize = quint64(decoded.size());
    entry.format = ModuleFormat::LZIP;
    entry.hierarchyPath = QStringList() << entry.name;
    entry.isEmbeddedFile = true;
    entry.content = std::make_shared<fs::MemoryStore>(std::move(decoded));
    module->resources_.push_back(std::move(entry));
    return module;
}

} // namespace peare
