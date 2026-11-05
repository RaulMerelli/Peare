#include "UnixCompressModule.h"
#include "Compat.h"
#include "../fs/QFileByteStore.h"

#include <QFileInfo>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace peare {
namespace {

const int kClearCode = 256;
const int kFirstDictionaryCode = 257;
const int kMaximumCodeBits = 16;

class CodeReader {
public:
    CodeReader(const std::uint8_t* data, std::size_t size, std::size_t offset)
        : data_(data), size_(size), position_(offset)
    {
    }

    bool read(int width, int* code)
    {
        while (availableBits_ < width) {
            if (position_ >= size_) return false;
            bitBuffer_ |= std::uint32_t(data_[position_++]) << availableBits_;
            availableBits_ += 8;
            ++bytesInSection_;
        }
        const std::uint32_t mask = (std::uint32_t(1) << width) - 1;
        *code = int(bitBuffer_ & mask);
        bitBuffer_ >>= width;
        availableBits_ -= width;
        return true;
    }

    bool resetSection(int width)
    {
        const std::uint64_t remainder = bytesInSection_ % std::uint64_t(width);
        const std::size_t skip = std::size_t((std::uint64_t(width) - remainder) % width);
        bitBuffer_ = 0;
        availableBits_ = 0;
        if (skip > size_ - position_) return false;
        position_ += skip;
        bytesInSection_ = 0;
        return true;
    }

    void beginNewWidthSection() { bytesInSection_ = 0; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_;
    std::uint32_t bitBuffer_ = 0;
    int availableBits_ = 0;
    std::uint64_t bytesInSection_ = 0;
};

bool decompressUnixCompress(const std::vector<std::uint8_t>& input,
                            std::vector<std::uint8_t>* output)
{
    if (!output || input.size() < 3) return false;
    const auto* data = input.data();
    if (data[0] != 0x1f || data[1] != 0x9d) return false;

    const std::uint8_t flags = data[2];
    const int maxBits = flags & 0x1f;
    const bool blockMode = (flags & 0x80) != 0;
    if (maxBits < 9 || maxBits > kMaximumCodeBits || (flags & 0x60) != 0)
        return false;

    const int maxCodes = 1 << maxBits;
    int codeWidth = 9;
    int sectionEnd = (1 << codeWidth) - 1;
    int nextCode = blockMode ? kFirstDictionaryCode : kClearCode;
    int previousCode = -1;
    std::uint8_t firstByte = 0;
    std::vector<std::uint16_t> prefixes(std::size_t(maxCodes), 0);
    std::vector<std::uint8_t> suffixes(std::size_t(maxCodes), 0);
    std::vector<std::uint8_t> stack(std::size_t(maxCodes) + 1, 0);
    CodeReader reader(data, input.size(), 3);
    std::vector<std::uint8_t> decoded;

    for (int code = 0; code < 256; ++code)
        suffixes[std::size_t(code)] = std::uint8_t(code);

    for (;;) {
        int code = 0;
        if (!reader.read(codeWidth, &code)) break;

        if (blockMode && code == kClearCode) {
            if (!reader.resetSection(codeWidth)) return false;
            codeWidth = 9;
            sectionEnd = (1 << codeWidth) - 1;
            nextCode = kFirstDictionaryCode;
            previousCode = -1;
            continue;
        }

        if (code > nextCode || (code == nextCode && previousCode < 0))
            return false;

        const int inputCode = code;
        std::size_t stackSize = 0;
        if (code == nextCode) {
            if (previousCode < 0 || stackSize >= stack.size()) return false;
            stack[stackSize++] = firstByte;
            code = previousCode;
        }

        while (code >= 256) {
            if (code >= nextCode || stackSize >= stack.size()) return false;
            stack[stackSize++] = suffixes[std::size_t(code)];
            code = prefixes[std::size_t(code)];
        }
        if (code < 0 || code > 255 || stackSize >= stack.size()) return false;
        firstByte = std::uint8_t(code);
        stack[stackSize++] = firstByte;

        if (stackSize > decoded.max_size() - decoded.size()) return false;
        std::reverse(stack.begin(), stack.begin() + stackSize);
        decoded.insert(decoded.end(), stack.begin(), stack.begin() + stackSize);

        if (previousCode >= 0 && nextCode < maxCodes) {
            prefixes[std::size_t(nextCode)] = std::uint16_t(previousCode);
            suffixes[std::size_t(nextCode)] = firstByte;
            ++nextCode;
            if (nextCode > sectionEnd && codeWidth < maxBits) {
                ++codeWidth;
                sectionEnd = codeWidth == maxBits ? maxCodes : (1 << codeWidth) - 1;
                reader.beginNewWidthSection();
            }
        }
        previousCode = inputCode;
    }

    *output = std::move(decoded);
    return true;
}

QString payloadName(const QString& logicalName)
{
    QString name = QFileInfo(logicalName).fileName();
    if (name.endsWith(QStringLiteral(".Z"), Qt::CaseInsensitive)) name.chop(2);
    return name.isEmpty() ? QStringLiteral("compress_payload") : name;
}

}  // namespace

ModulePtr UnixCompressModule::open(const QString& filePath)
{
    auto file = std::make_shared<fs::QFileByteStore>(filePath);
    if (!file->validate()) {
        auto module = peare::makeUnique<UnixCompressModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::UNIX_COMPRESS;
        module->info_.error = QStringLiteral("Could not read Unix compress input");
        return ModulePtr(std::move(module));
    }
    return open(file, filePath);
}

ModulePtr UnixCompressModule::open(const QByteArray& data, const QString& logicalName)
{
    return open(std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(data.constData()),
        reinterpret_cast<const std::uint8_t*>(data.constData()) + data.size()), logicalName);
}

ModulePtr UnixCompressModule::open(std::vector<std::uint8_t> data,
                                   const QString& logicalName)
{
    auto module = peare::makeUnique<UnixCompressModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::UNIX_COMPRESS;
    module->info_.description = QStringLiteral("Unix compress (.Z) stream");

    std::vector<std::uint8_t> decoded;
    if (!decompressUnixCompress(data, &decoded)) {
        module->info_.error = QStringLiteral("Invalid or truncated Unix compress stream");
        return ModulePtr(std::move(module));
    }

    ResourceEntry entry;
    entry.type = QStringLiteral("UNIX_COMPRESS_FILE");
    entry.name = payloadName(logicalName);
    entry.language = QStringLiteral("neutral");
    entry.dataSize = quint64(decoded.size());
    entry.format = ModuleFormat::UNIX_COMPRESS;
    entry.hierarchyPath = QStringList() << entry.name;
    entry.isEmbeddedFile = true;
    entry.content = std::make_shared<fs::MemoryStore>(std::move(decoded));
    module->resources_.push_back(std::move(entry));
    return ModulePtr(std::move(module));
}

ModulePtr UnixCompressModule::open(const fs::ByteStorePtr& file, const QString& logicalName)
{
    if (!file || file->capacity() < 0 ||
        std::uint64_t(file->capacity()) > std::numeric_limits<std::size_t>::max()) {
        auto module = peare::makeUnique<UnixCompressModule>();
        module->info_.filePath = logicalName;
        module->info_.format = ModuleFormat::UNIX_COMPRESS;
        module->info_.error = QStringLiteral("Invalid Unix compress byte source");
        return ModulePtr(std::move(module));
    }
    const std::vector<std::uint8_t> bytes = file->readAll();
    return open(std::move(bytes), logicalName);
}

}  // namespace peare
