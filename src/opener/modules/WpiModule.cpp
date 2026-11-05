#include "WpiModule.h"
#include "Compat.h"
#include "../fs/QFileByteStore.h"

#include "../compression/peare/Bzip2Decoder.h"
#include "../fs/DiscStore.h"

#include <QFile>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace peare {
namespace {

const std::int64_t kWpiHeaderSize = 532;
const std::int64_t kWpiExtendedHeaderSize = 40;
const std::int64_t kWpiFileHeaderSize = 285;
const std::int64_t kWpiPackHeaderSize = 48;
const std::uint16_t kWpiFileMagic = 0xF012;

std::uint16_t readLe16(const std::uint8_t* p) {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}

std::uint32_t readLe32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::int32_t readLeI32(const std::uint8_t* p) {
    return static_cast<std::int32_t>(readLe32(p));
}

QString fixedString(const std::uint8_t* p, int size) {
    int length = 0;
    while (length < size && p[length] != 0) ++length;
    return QString::fromLatin1(reinterpret_cast<const char*>(p), length).trimmed();
}

bool safePath(const QString& raw, QStringList* hierarchy, QString* leaf) {
    QString normalized = raw;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (normalized.isEmpty() || normalized.startsWith(QLatin1Char('/')) ||
        normalized.contains(QLatin1Char(':'))) return false;
    const QStringList parts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return false;
    for (const QString& part : parts)
        if (part == QLatin1String(".") || part == QLatin1String("..")) return false;
    *hierarchy = parts;
    *leaf = hierarchy->takeLast();
    return !leaf->isEmpty();
}

std::uint32_t wpiCrc(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEBD88320U) : (crc >> 1);
    }
    return crc;
}

bool decompressBzip(const QByteArray& input, std::uint32_t expectedSize,
                    QByteArray* output) {
    if (!output || expectedSize > std::uint32_t((std::numeric_limits<int>::max)())) return false;
    std::vector<std::uint8_t> decoded;
    if (!compression::decompressBzip2(
            reinterpret_cast<const std::uint8_t*>(input.constData()),
            std::size_t(input.size()), expectedSize, false, &decoded) ||
        decoded.size() != expectedSize)
        return false;
    *output = QByteArray(reinterpret_cast<const char*>(decoded.data()),
                         static_cast<int>(decoded.size()));
    return true;
}

class WpiPayloadStore final : public fs::IByteStore {
public:
    WpiPayloadStore(fs::ByteStorePtr source, std::int64_t offset, std::uint32_t compressedSize,
                    std::uint32_t originalSize, std::uint16_t method, std::uint32_t crc)
        : source_(std::move(source)), offset_(offset), compressedSize_(compressedSize),
          originalSize_(originalSize), method_(method), crc_(crc) {}

    std::int64_t capacity() const override { return originalSize_; }
    bool cheapRandomAccess() const override { return false; }
    bool validate() const override { return load(); }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || pos >= originalSize_ || !load()) return 0;
        const int n = static_cast<int>(std::min<std::int64_t>(count, originalSize_ - pos));
        std::memcpy(dst, decoded_.data() + pos, std::size_t(n));
        return n;
    }

private:
    bool load() const {
        std::call_once(loadOnce_, [this] {
            if (!source_) return;
            const std::vector<std::uint8_t> bytes = source_->readRange(offset_, compressedSize_);
            if (bytes.size() != compressedSize_) return;
            if (method_ == 0) {
                decoded_ = bytes;
            } else if (method_ == 1) {
                if (!compression::decompressBzip2(
                        bytes.data(), bytes.size(), originalSize_, false, &decoded_))
                    return;
            } else {
                return;
            }
            valid_ = decoded_.size() == originalSize_;
            if (valid_ && method_ == 0 && crc_ != 0)
                valid_ = (wpiCrc(decoded_) ^ 0xFFFFFFFFU) == crc_;
            if (!valid_) decoded_.clear();
        });
        return valid_;
    }

    fs::ByteStorePtr source_;
    std::int64_t offset_;
    std::uint32_t compressedSize_;
    std::uint32_t originalSize_;
    std::uint16_t method_;
    std::uint32_t crc_;
    mutable std::once_flag loadOnce_;
    mutable bool valid_ = false;
    mutable std::vector<std::uint8_t> decoded_;
};

fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), size,
            std::static_pointer_cast<void>(holder));
    auto source = std::make_shared<fs::QFileByteStore>(path);
    return source->validate() ? source : fs::ByteStorePtr();
}

}  // namespace

ModulePtr WpiModule::open(const QString& filePath) {
    auto module = open(storeForFile(filePath), filePath);
    return module;
}

ModulePtr WpiModule::open(const QByteArray& data, const QString& logicalName) {
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(data.constData()),
                    std::size_t(data.size())), logicalName);
}

ModulePtr WpiModule::open(const fs::ByteStorePtr& file, const QString& sourceName) {
    auto module = peare::makeUnique<WpiModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::WPI;
    module->info_.description = QStringLiteral("OS/2 WarpIN package");
    const auto fail = [&module](const QString& error) {
        module->info_.error = error;
        module->resources_.clear();
        return ModulePtr(std::move(module));
    };
    if (!file || file->capacity() < kWpiHeaderSize)
        return fail(QStringLiteral("Truncated WarpIN archive header"));

    const std::vector<std::uint8_t> header = file->readRange(0, kWpiHeaderSize);
    if (header.size() != std::size_t(kWpiHeaderSize) ||
        std::memcmp(header.data(), "\x77\x04\x02\xBE", 4) != 0)
        return fail(QStringLiteral("Invalid WarpIN archive signature"));
    const std::int16_t revision = static_cast<std::int16_t>(readLe16(header.data() + 4));
    if (revision != 3 && revision != 4)
        return fail(QStringLiteral("Unsupported WarpIN archive revision"));
    const std::int16_t packageCount = static_cast<std::int16_t>(readLe16(header.data() + 522));
    const std::uint16_t scriptSize = readLe16(header.data() + 524);
    const std::uint16_t compressedScriptSize = readLe16(header.data() + 526);
    const std::int32_t extendedSize = readLeI32(header.data() + 528);
    if (packageCount < 0 || packageCount > 4096 || extendedSize < 0 ||
        std::int64_t(scriptSize) + compressedScriptSize + extendedSize > file->capacity())
        return fail(QStringLiteral("Invalid WarpIN archive lengths"));

    std::int64_t cursor = kWpiHeaderSize;
    if (revision == 4) {
        const std::vector<std::uint8_t> ext = file->readRange(cursor, kWpiExtendedHeaderSize);
        if (ext.size() != std::size_t(kWpiExtendedHeaderSize) ||
            readLeI32(ext.data()) != kWpiExtendedHeaderSize)
            return fail(QStringLiteral("Invalid WarpIN extended header"));
        cursor += kWpiExtendedHeaderSize;
    }

    if (compressedScriptSize > 0) {
        const std::vector<std::uint8_t> compressed = file->readRange(cursor, compressedScriptSize);
        QByteArray script;
        if (compressed.size() != compressedScriptSize ||
            !decompressBzip(QByteArray(reinterpret_cast<const char*>(compressed.data()),
                                       int(compressed.size())), scriptSize, &script))
            return fail(QStringLiteral("Invalid WarpIN installation script"));
        ResourceEntry entry;
        entry.type = QStringLiteral("WPI_SCRIPT");
        entry.name = QStringLiteral("Installation script");
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = quint64(cursor);
        entry.dataSize = quint64(script.size());
        entry.format = ModuleFormat::WPI;
        entry.isOs2 = true;
        entry.data = std::move(script);
        module->resources_.push_back(std::move(entry));
        cursor += compressedScriptSize;
    }
    cursor += extendedSize;

    struct PackageInfo { std::int16_t number; std::int16_t files; std::int32_t offset; QString name; };
    std::map<std::int16_t, PackageInfo> packages;
    if (cursor + std::int64_t(packageCount) * kWpiPackHeaderSize > file->capacity())
        return fail(QStringLiteral("Truncated WarpIN package table"));
    for (std::int16_t i = 0; i < packageCount; ++i) {
        const std::vector<std::uint8_t> pack = file->readRange(cursor, kWpiPackHeaderSize);
        if (pack.size() != std::size_t(kWpiPackHeaderSize))
            return fail(QStringLiteral("Truncated WarpIN package record"));
        PackageInfo info;
        info.number = static_cast<std::int16_t>(readLe16(pack.data()));
        info.files = static_cast<std::int16_t>(readLe16(pack.data() + 2));
        info.offset = readLeI32(pack.data() + 4);
        info.name = fixedString(pack.data() + 16, 32);
        if (info.number < 0 || info.files < 0 || info.offset < 0 ||
            info.offset >= file->capacity() || packages.count(info.number))
            return fail(QStringLiteral("Invalid WarpIN package record"));
        packages.emplace(info.number, info);
        cursor += kWpiPackHeaderSize;
    }

    for (const auto& packagePair : packages) {
        const PackageInfo& package = packagePair.second;
        std::int64_t recordOffset = package.offset;
        for (std::int16_t i = 0; i < package.files; ++i) {
            const std::vector<std::uint8_t> raw = file->readRange(recordOffset, kWpiFileHeaderSize);
            if (raw.size() != std::size_t(kWpiFileHeaderSize) ||
                readLe16(raw.data()) != kWpiFileMagic)
                return fail(QStringLiteral("Invalid WarpIN file record"));
            const std::int16_t method = static_cast<std::int16_t>(readLe16(raw.data() + 4));
            const std::int16_t owner = static_cast<std::int16_t>(readLe16(raw.data() + 6));
            const std::int32_t originalSize = readLeI32(raw.data() + 8);
            const std::int32_t compressedSize = readLeI32(raw.data() + 12);
            const std::uint32_t crc = readLe32(raw.data() + 16);
            const QString rawName = fixedString(raw.data() + 20, 256);
            const std::uint8_t extraSize = raw[284];
            QStringList hierarchy;
            QString leaf;
            if ((method != 0 && method != 1) || owner != package.number || originalSize < 0 ||
                compressedSize < 0 || !safePath(rawName, &hierarchy, &leaf))
                return fail(QStringLiteral("Invalid WarpIN file metadata"));
            const std::int64_t payloadOffset = recordOffset + kWpiFileHeaderSize + extraSize;
            if (payloadOffset < recordOffset || payloadOffset + compressedSize > file->capacity())
                return fail(QStringLiteral("Truncated WarpIN file payload"));
            if (!package.name.isEmpty()) hierarchy.prepend(package.name);

            auto content = std::make_shared<WpiPayloadStore>(
                file, payloadOffset, std::uint32_t(compressedSize),
                std::uint32_t(originalSize), std::uint16_t(method), crc);
            ResourceEntry entry;
            entry.type = QStringLiteral("WPI_FILE");
            entry.isEmbeddedFile = true;
            entry.name = leaf;
            entry.language = QStringLiteral("neutral");
            entry.dataOffset = quint64(payloadOffset);
            entry.dataSize = quint64(originalSize);
            entry.format = ModuleFormat::WPI;
            entry.isOs2 = true;
            entry.hierarchyPath = hierarchy;
            entry.content = std::move(content);
            module->resources_.push_back(std::move(entry));
            recordOffset = payloadOffset + compressedSize;
        }
    }
    return ModulePtr(std::move(module));
}

}  // namespace peare
