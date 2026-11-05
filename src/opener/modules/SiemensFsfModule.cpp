#include "SiemensFsfModule.h"
#include "Compat.h"
#include "DeflateDecoder.h"

#include <QFile>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace peare {
namespace {

const std::int64_t kFsfHeaderSize = 20;
const std::int64_t kRecordPrefixSize = 11;
const std::uint16_t kFileRecordType = 1;
const std::uint16_t kFileRecordVersion = 1;
const std::uint32_t kMaxRecordCount = 1000000;
const std::uint16_t kMaxRecordHeader = 0x4000;
const std::uint32_t kMaxMemberBytes = 512U * 1024U * 1024U;

std::uint16_t readBe16(const std::uint8_t* p) {
    return (std::uint16_t(p[0]) << 8) | std::uint16_t(p[1]);
}

std::uint32_t readBe32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

std::uint32_t adler32(const std::vector<std::uint8_t>& bytes) {
    const std::uint32_t mod = 65521;
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t byte : bytes) {
        a = (a + byte) % mod;
        b = (b + a) % mod;
    }
    return (b << 16) | a;
}

bool inflateMember(const fs::ByteStorePtr& file, std::int64_t offset,
                   std::uint32_t expectedSize, std::vector<std::uint8_t>& output,
                   std::size_t& consumed) {
    if (expectedSize > kMaxMemberBytes || offset < 0 || offset > file->capacity() - 6)
        return false;
    const std::vector<std::uint8_t> input = file->readRange(
        offset, file->capacity() - offset);
    if (input.size() < 6 || (input[0] & 0x0fU) != 8U || (input[0] >> 4) > 7U ||
        (((unsigned(input[0]) << 8) | input[1]) % 31U) != 0U ||
        (input[1] & 0x20U) != 0U)
        return false;

    compression::MemoryInflateInput deflate(input.data() + 2, input.size() - 2);
    std::string error;
    if (!compression::inflateRawExact(deflate, expectedSize, nullptr, &output, &error))
        return false;
    consumed = 2 + deflate.consumed() + 4;
    if (consumed > input.size()) {
        output.clear();
        return false;
    }
    const std::size_t trailer = consumed - 4;
    const std::uint32_t expectedAdler = (std::uint32_t(input[trailer]) << 24) |
        (std::uint32_t(input[trailer + 1]) << 16) |
        (std::uint32_t(input[trailer + 2]) << 8) | input[trailer + 3];
    if (adler32(output) != expectedAdler) {
        output.clear();
        return false;
    }
    return true;
}

fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    const QByteArray bytes = holder->readAll();
    return std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
}

QString normalizePath(const std::vector<std::uint8_t>& raw) {
    QString path = QString::fromLatin1(reinterpret_cast<const char*>(raw.data()),
                                      static_cast<int>(raw.size()));
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.startsWith(QLatin1Char('/'))) path.remove(0, 1);
    return path;
}

bool splitSafePath(const QString& normalized, QStringList& hierarchy, QString& leaf) {
    const QStringList parts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return false;
    for (const QString& part : parts) {
        if (part == QLatin1String(".") || part == QLatin1String("..")) return false;
        for (const QChar c : part)
            if (c.unicode() < 0x20) return false;
    }
    leaf = parts.last();
    hierarchy = parts;
    hierarchy.removeLast();
    return !leaf.isEmpty();
}

struct ParseState {
    std::uint32_t declaredPayloadBytes = 0;
    std::uint32_t declaredRecords = 0;
    std::uint32_t parsedRecords = 0;
    std::uint32_t completeFiles = 0;
    std::uint64_t completePayloadBytes = 0;
    bool truncated = false;
    QString truncatedName;
    QString error;
};

ParseState parseFsf(const fs::ByteStorePtr& file, QVector<ResourceEntry>& resources) {
    ParseState state;
    if (!file || file->capacity() < kFsfHeaderSize) {
        state.error = QStringLiteral("FSF header is truncated");
        return state;
    }

    const std::vector<std::uint8_t> header = file->readRange(0, kFsfHeaderSize);
    if (header.size() != std::size_t(kFsfHeaderSize) ||
        std::memcmp(header.data(), "FSF\0", 4) != 0) {
        state.error = QStringLiteral("Invalid Siemens FSF signature");
        return state;
    }

    // The first 12 bytes contain the FSF signature and implementation/version
    // fields. The two documented-by-observation archive counters are network
    // byte order: total member bytes at 0x0c and member count at 0x10.
    state.declaredPayloadBytes = readBe32(header.data() + 12);
    state.declaredRecords = readBe32(header.data() + 16);
    if (state.declaredRecords == 0 || state.declaredRecords > kMaxRecordCount) {
        state.error = QStringLiteral("Invalid Siemens FSF record count");
        return state;
    }

    std::int64_t pos = kFsfHeaderSize;
    for (std::uint32_t index = 0; index < state.declaredRecords; ++index) {
        if (pos + kRecordPrefixSize > file->capacity()) {
            state.truncated = true;
            break;
        }
        const std::vector<std::uint8_t> prefix = file->readRange(pos, kRecordPrefixSize);
        if (prefix.size() != std::size_t(kRecordPrefixSize)) {
            state.truncated = true;
            break;
        }

        const std::uint16_t type = readBe16(prefix.data());
        const std::uint16_t headerSize = readBe16(prefix.data() + 2);
        const std::uint16_t version = readBe16(prefix.data() + 4);
        const std::uint8_t storage = prefix[6];
        const std::uint16_t nameLength = readBe16(prefix.data() + 9);
        const std::uint32_t minimumHeader =
            std::uint32_t(kRecordPrefixSize) + std::uint32_t(nameLength) + 4U;
        if (headerSize < minimumHeader || headerSize > kMaxRecordHeader) {
            state.error = QStringLiteral("Invalid Siemens FSF record header at 0x%1")
                              .arg(qulonglong(pos), 0, 16);
            resources.clear();
            return state;
        }
        if (pos + headerSize > file->capacity()) {
            state.truncated = true;
            break;
        }

        const std::vector<std::uint8_t> recordHeader = file->readRange(pos, headerSize);
        if (recordHeader.size() != headerSize) {
            state.truncated = true;
            break;
        }
        std::vector<std::uint8_t> rawName(recordHeader.begin() + kRecordPrefixSize,
                                          recordHeader.begin() + kRecordPrefixSize + nameLength);
        const QString normalized = normalizePath(rawName);
        const std::uint32_t payloadSize = readBe32(recordHeader.data() + headerSize - 4);
        const std::int64_t payloadOffset = pos + headerSize;
        ++state.parsedRecords;

        QStringList hierarchy;
        QString leaf;
        if (!splitSafePath(normalized, hierarchy, leaf)) {
            state.error = QStringLiteral("Unsafe or empty Siemens FSF member path at record %1")
                              .arg(index);
            resources.clear();
            return state;
        }

        if (payloadOffset > file->capacity() || payloadSize > kMaxMemberBytes) {
            state.truncated = true;
            state.truncatedName = normalized;
            break;
        }

        std::vector<std::uint8_t> inflated;
        std::size_t storedSize = payloadSize;
        if (storage == 0) {
            if (std::uint64_t(payloadSize) >
                std::uint64_t(file->capacity() - payloadOffset)) {
                state.truncated = true;
                state.truncatedName = normalized;
                break;
            }
        } else if (!inflateMember(file, payloadOffset, payloadSize, inflated, storedSize)) {
            state.error = QStringLiteral("Invalid compressed Siemens FSF member: %1")
                              .arg(normalized);
            resources.clear();
            return state;
        }

        // Type 1/version 1 is the file-member record observed in Siemens HMI
        // images. Unknown records are skipped conservatively but still advance
        // using their explicit header and payload lengths.
        if (type == kFileRecordType && version == kFileRecordVersion) {
            ResourceEntry entry;
            entry.type = QStringLiteral("SIEMENS_FSF_FILE");
            entry.name = leaf;
            entry.language = QStringLiteral("neutral");
            entry.dataSize = quint64(payloadSize);
            entry.format = ModuleFormat::SIEMENS_FSF;
            entry.hierarchyPath = hierarchy;
            entry.dataOffset = quint64(payloadOffset);
            if (storage == 0) {
                entry.content = std::make_shared<fs::SubStore>(file, payloadOffset, payloadSize);
            } else {
                entry.data = QByteArray(reinterpret_cast<const char*>(inflated.data()),
                                        static_cast<int>(inflated.size()));
            }
            entry.isEmbeddedFile = true;
            resources.push_back(std::move(entry));
            ++state.completeFiles;
            state.completePayloadBytes += payloadSize;
        }

        if (std::uint64_t(payloadOffset) + storedSize >
            std::uint64_t(std::numeric_limits<std::int64_t>::max())) {
            state.error = QStringLiteral("Siemens FSF member offset overflow");
            resources.clear();
            return state;
        }
        pos = payloadOffset + std::int64_t(storedSize);
    }

    if (resources.isEmpty() && state.error.isEmpty()) {
        state.error = state.truncated
            ? QStringLiteral("Siemens FSF is truncated before the first complete file")
            : QStringLiteral("Siemens FSF contains no supported file records");
    }
    return state;
}

} // namespace

ModulePtr SiemensFsfModule::open(const QString& filePath) {
    return open(storeForFile(filePath), filePath);
}

ModulePtr SiemensFsfModule::open(const QByteArray& data, const QString& logicalName) {
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(data.constData()),
                    std::size_t(data.size())),
                logicalName);
}

ModulePtr SiemensFsfModule::open(const fs::ByteStorePtr& file, const QString& sourceName) {
    auto module = peare::makeUnique<SiemensFsfModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::SIEMENS_FSF;
    module->info_.description = QStringLiteral("Siemens FSF Windows CE flash-file archive");
    module->file_ = file;

    const ParseState state = parseFsf(file, module->resources_);
    if (!state.error.isEmpty()) {
        module->info_.error = state.error;
        return ModulePtr(std::move(module));
    }

    if (state.truncated) {
        QString detail = QStringLiteral("%1/%2 complete files")
                             .arg(state.completeFiles)
                             .arg(state.declaredRecords);
        if (!state.truncatedName.isEmpty())
            detail += QStringLiteral("; truncated while reading %1").arg(state.truncatedName);
        module->info_.description += QStringLiteral(" (%1)").arg(detail);
    } else {
        module->info_.description += QStringLiteral(" (%1 files)").arg(state.completeFiles);
    }
    return ModulePtr(std::move(module));
}

} // namespace peare
