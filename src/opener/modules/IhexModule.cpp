#include "IhexModule.h"
#include "Compat.h"
#include "../fs/QFileByteStore.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

namespace peare {
namespace {

struct IhexRecord {
    std::uint16_t address = 0;
    std::uint8_t type = 0;
    std::vector<std::uint8_t> data;
};

struct Segment {
    std::uint64_t address = 0;
    std::vector<std::uint8_t> data;
};

int hexDigit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

bool parseRecord(QByteArray line, IhexRecord* record)
{
    if (!record) return false;
    if (line.endsWith('\r')) line.chop(1);
    if (line.size() < 11 || line.at(0) != ':' || ((line.size() - 1) & 1)) return false;

    std::vector<std::uint8_t> bytes;
    bytes.reserve(std::size_t((line.size() - 1) / 2));
    for (int i = 1; i < line.size(); i += 2) {
        const int high = hexDigit(line.at(i));
        const int low = hexDigit(line.at(i + 1));
        if (high < 0 || low < 0) return false;
        bytes.push_back(std::uint8_t((high << 4) | low));
    }
    if (bytes.size() < 5 || bytes.size() != std::size_t(bytes[0]) + 5) return false;
    unsigned sum = 0;
    for (std::uint8_t value : bytes) sum += value;
    if ((sum & 0xff) != 0) return false;

    record->address = std::uint16_t((std::uint16_t(bytes[1]) << 8) | bytes[2]);
    record->type = bytes[3];
    record->data.assign(bytes.begin() + 4, bytes.end() - 1);
    return true;
}

bool parseFirstRecord(const QByteArray& data, IhexRecord* record)
{
    qsizetype pos = 0;
    while (pos < data.size()) {
        const qsizetype end = data.indexOf('\n', pos);
        QByteArray line = data.mid(pos, end < 0 ? -1 : end - pos);
        pos = end < 0 ? data.size() : end + 1;
        if (line.startsWith("\xEF\xBB\xBF")) line.remove(0, 3);
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(';')) continue;
        if (!parseRecord(line, record)) return false;
        return record->type <= 5;
    }
    return false;
}

bool insertRecord(std::map<std::uint64_t, Segment>* segments,
                  std::uint64_t address, const std::vector<std::uint8_t>& data)
{
    if (data.empty()) return true;
    if (address > 0x100000000ull || data.size() > 0x100000000ull - address)
        return false;
    const std::uint64_t end = address + data.size();
    auto next = segments->lower_bound(address);
    if (next != segments->begin()) {
        auto previous = std::prev(next);
        const std::uint64_t previousEnd = previous->first + previous->second.data.size();
        if (previousEnd > address) return false;
        if (previousEnd == address) {
            previous->second.data.insert(previous->second.data.end(), data.begin(), data.end());
            if (next != segments->end() && end == next->first) {
                previous->second.data.insert(previous->second.data.end(),
                                             next->second.data.begin(), next->second.data.end());
                segments->erase(next);
            } else if (next != segments->end() && end > next->first) {
                return false;
            }
            return true;
        }
    }
    if (next != segments->end() && end > next->first) return false;
    Segment segment;
    segment.address = address;
    segment.data = data;
    if (next != segments->end() && end == next->first) {
        segment.data.insert(segment.data.end(), next->second.data.begin(), next->second.data.end());
        segments->erase(next);
    }
    segments->emplace(address, std::move(segment));
    return true;
}

bool readExact(const fs::IByteStore& source, std::int64_t offset,
               std::uint8_t* buffer, int count)
{
    int done = 0;
    while (done < count) {
        const int received = source.read(offset + done, buffer + done, count - done);
        if (received <= 0 || received > count - done) return false;
        done += received;
    }
    return true;
}

bool parseSource(const fs::ByteStorePtr& source, QVector<ResourceEntry>* resources,
                 QString* error)
{
    if (!source || !resources || source->capacity() <= 0) {
        *error = QStringLiteral("Empty or unavailable Intel HEX input");
        return false;
    }

    std::map<std::uint64_t, Segment> segments;
    std::uint64_t base = 0;
    std::int64_t offset = 0;
    QByteArray line;
    std::vector<std::uint8_t> chunk(64 * 1024);
    bool sawRecord = false;
    bool sawEof = false;
    bool sawStart = false;
    std::uint8_t startType = 0;
    std::uint64_t startAddress = 0;
    std::vector<std::uint8_t> startData;

    auto consumeLine = [&]() -> bool {
        if (line.endsWith('\r')) line.chop(1);
        if (line.startsWith("\xEF\xBB\xBF")) line.remove(0, 3);
        const QByteArray trimmed = line.trimmed();
        line.clear();
        if (trimmed.isEmpty() || trimmed.startsWith(';')) return true;
        if (sawEof) return false;

        IhexRecord record;
        if (!parseRecord(trimmed, &record)) return false;
        sawRecord = true;
        switch (record.type) {
        case 0: {
            const std::uint64_t address = base + record.address;
            return insertRecord(&segments, address, record.data);
        }
        case 1:
            if (record.address != 0 || !record.data.empty()) return false;
            sawEof = true;
            return true;
        case 2:
            if (record.address != 0 || record.data.size() != 2) return false;
            base = ((std::uint64_t(record.data[0]) << 8) | record.data[1]) << 4;
            return true;
        case 3:
        case 5:
            if (record.address != 0 || record.data.size() != 4 || sawStart) return false;
            sawStart = true;
            startType = record.type;
            if (record.type == 3) {
                const std::uint32_t segment =
                    (std::uint32_t(record.data[0]) << 8) | record.data[1];
                const std::uint32_t offset =
                    (std::uint32_t(record.data[2]) << 8) | record.data[3];
                startAddress = std::uint64_t(segment) * 16 + offset;
            } else {
                startAddress = (std::uint32_t(record.data[0]) << 24) |
                    (std::uint32_t(record.data[1]) << 16) |
                    (std::uint32_t(record.data[2]) << 8) | record.data[3];
            }
            startData.swap(record.data);
            return true;
        case 4:
            if (record.address != 0 || record.data.size() != 2) return false;
            base = ((std::uint64_t(record.data[0]) << 8) | record.data[1]) << 16;
            return true;
        default:
            return false;
        }
    };

    while (offset < source->capacity()) {
        const int request = int(std::min<std::int64_t>(
            std::int64_t(chunk.size()), source->capacity() - offset));
        if (!readExact(*source, offset, chunk.data(), request)) {
            *error = QStringLiteral("Could not read Intel HEX input");
            return false;
        }
        for (int i = 0; i < request; ++i) {
            const char value = char(chunk[std::size_t(i)]);
            if (value == '\n') {
                if (!consumeLine()) {
                    *error = QStringLiteral("Invalid Intel HEX record or address overlap");
                    return false;
                }
            } else {
                if (line.size() >= 1024) {
                    *error = QStringLiteral("Intel HEX record exceeds the line limit");
                    return false;
                }
                line.append(value);
            }
        }
        offset += request;
    }
    if (!line.isEmpty() && !consumeLine()) {
        *error = QStringLiteral("Invalid final Intel HEX record");
        return false;
    }
    if (!sawRecord || !sawEof) {
        *error = QStringLiteral("Intel HEX input has no complete end-of-file record");
        return false;
    }

    const std::size_t maxEntrySize = 256 * 1024 * 1024;
    for (const auto& item : segments) {
        for (std::size_t pos = 0; pos < item.second.data.size(); pos += maxEntrySize) {
            const std::size_t length = std::min(maxEntrySize, item.second.data.size() - pos);
            const std::uint64_t address = item.first + pos;
            ResourceEntry entry;
            entry.type = QStringLiteral("IHEX_DATA");
            entry.name = QStringLiteral("0x%1.bin").arg(
                qulonglong(address), 8, 16, QLatin1Char('0'));
            entry.dataOffset = address;
            entry.dataSize = length;
            entry.format = ModuleFormat::INTEL_HEX;
            entry.data = QByteArray(
                reinterpret_cast<const char*>(item.second.data.data() + pos), int(length));
            entry.isEmbeddedFile = true;
            resources->append(std::move(entry));
        }
    }
    if (sawStart) {
        ResourceEntry entry;
        entry.type = QStringLiteral("IHEX_START_ADDRESS");
        entry.name = QStringLiteral("0x%1 (%2)").arg(
            qulonglong(startAddress), 8, 16, QLatin1Char('0')).arg(
                startType == 3 ? QStringLiteral("segment") : QStringLiteral("linear"));
        entry.dataSize = startData.size();
        entry.format = ModuleFormat::INTEL_HEX;
        entry.data = QByteArray(reinterpret_cast<const char*>(startData.data()),
                                int(startData.size()));
        resources->append(std::move(entry));
    }
    if (resources->isEmpty()) {
        ResourceEntry empty;
        empty.type = QStringLiteral("IHEX_DATA");
        empty.name = QStringLiteral("0x00000000.bin");
        empty.format = ModuleFormat::INTEL_HEX;
        empty.isEmbeddedFile = true;
        resources->append(std::move(empty));
    }
    return true;
}

}  // namespace

bool IhexModule::hasHeader(const QByteArray& data)
{
    IhexRecord record;
    return parseFirstRecord(data, &record);
}

ModulePtr IhexModule::open(const QString& filePath)
{
    return open(std::make_shared<fs::QFileByteStore>(filePath), filePath);
}

ModulePtr IhexModule::open(const fs::ByteStorePtr& file, const QString& logicalName)
{
    auto module = peare::makeUnique<IhexModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::INTEL_HEX;
    module->info_.description = QStringLiteral("Intel HEX firmware records");
    if (!file || !file->validate()) {
        module->info_.error = QStringLiteral("Could not read Intel HEX input");
        return ModulePtr(std::move(module));
    }
    if (!parseSource(file, &module->resources_, &module->info_.error))
        module->resources_.clear();
    return ModulePtr(std::move(module));
}

}  // namespace peare
