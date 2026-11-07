#include "ZstdModule.h"
#include "Compat.h"

#include "../compression/third_party/zstd/zstd.h"
#include "../fs/DiscStore.h"
#include "../fs/QFileByteStore.h"

#include <QFileInfo>

#include <algorithm>
#include <limits>
#include <vector>

namespace peare {
namespace {

constexpr std::uint8_t kZstdMagic[] = {0x28, 0xB5, 0x2F, 0xFD};

bool hasZstdHeader(const std::vector<std::uint8_t>& data)
{
    if (data.size() < 4) return false;
    if (std::equal(std::begin(kZstdMagic), std::end(kZstdMagic), data.begin())) return true;
    return (data[0] & 0xF0u) == 0x50u && data[1] == 0x2A &&
        data[2] == 0x4D && data[3] == 0x18;
}

QString payloadName(const QString& logicalName)
{
    QString name = QFileInfo(logicalName).fileName();
    if (name.endsWith(QStringLiteral(".zst"), Qt::CaseInsensitive)) name.chop(4);
    return name.isEmpty() ? QStringLiteral("zstd_payload") : name;
}

bool decompressFrames(const std::vector<std::uint8_t>& input,
                      std::vector<std::uint8_t>* output)
{
    if (!output || !hasZstdHeader(input)) return false;
    ZSTD_DStream* stream = ZSTD_createDStream();
    if (!stream) return false;
    const int maxWindowLog = sizeof(std::size_t) == 4 ? 30 : 31;
    const std::size_t windowResult =
        ZSTD_DCtx_setParameter(stream, ZSTD_d_windowLogMax, maxWindowLog);
    if (ZSTD_isError(windowResult)) {
        ZSTD_freeDStream(stream);
        return false;
    }
    const std::size_t initResult = ZSTD_initDStream(stream);
    if (ZSTD_isError(initResult)) {
        ZSTD_freeDStream(stream);
        return false;
    }

    ZSTD_inBuffer in = {input.data(), input.size(), 0};
    std::vector<std::uint8_t> decoded;
    std::vector<std::uint8_t> chunk(ZSTD_DStreamOutSize());
    std::size_t result = 1;
    bool valid = true;
    while (in.pos < in.size) {
        ZSTD_outBuffer out = {chunk.data(), chunk.size(), 0};
        const std::size_t oldPosition = in.pos;
        result = ZSTD_decompressStream(stream, &out, &in);
        if (ZSTD_isError(result) ||
            out.pos > decoded.max_size() - decoded.size()) {
            valid = false;
            break;
        }
        decoded.insert(decoded.end(), chunk.begin(), chunk.begin() + std::ptrdiff_t(out.pos));
        if (result != 0 && in.pos == in.size) {
            valid = false;
            break;
        }
        if (in.pos == oldPosition && out.pos == 0 && result != 0) {
            valid = false;
            break;
        }
    }
    ZSTD_freeDStream(stream);
    if (!valid || in.pos != in.size || result != 0) return false;
    output->swap(decoded);
    return true;
}

} // namespace

bool ZstdModule::decodePayload(const std::vector<std::uint8_t>& input,
                               std::vector<std::uint8_t>* output)
{
    return decompressFrames(input, output);
}

std::unique_ptr<ZstdModule> ZstdModule::open(const QString& filePath)
{
    auto source = std::make_shared<fs::QFileByteStore>(filePath);
    if (!source->validate()) {
        auto module = peare::makeUnique<ZstdModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::ZSTD;
        module->info_.error = QStringLiteral("Could not read Zstandard input");
        return module;
    }
    return open(source, filePath);
}

std::unique_ptr<ZstdModule> ZstdModule::open(const fs::ByteStorePtr& source,
                                             const QString& logicalName)
{
    if (!source || source->capacity() < 0 ||
        std::uint64_t(source->capacity()) > std::numeric_limits<std::size_t>::max()) {
        auto module = peare::makeUnique<ZstdModule>();
        module->info_.filePath = logicalName;
        module->info_.format = ModuleFormat::ZSTD;
        module->info_.error = QStringLiteral("Invalid Zstandard byte source");
        return module;
    }
    return open(source->readAll(), logicalName);
}

std::unique_ptr<ZstdModule> ZstdModule::open(const QByteArray& data,
                                             const QString& logicalName)
{
    const auto* begin = reinterpret_cast<const std::uint8_t*>(data.constData());
    return open(std::vector<std::uint8_t>(begin, begin + data.size()), logicalName);
}

std::unique_ptr<ZstdModule> ZstdModule::open(std::vector<std::uint8_t> data,
                                             const QString& logicalName)
{
    auto module = peare::makeUnique<ZstdModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::ZSTD;
    module->info_.description = QStringLiteral("Zstandard compressed stream");

    std::vector<std::uint8_t> decoded;
    if (!decompressFrames(data, &decoded)) {
        module->info_.error = QStringLiteral("Invalid, truncated or unsupported Zstandard stream");
        return module;
    }

    ResourceEntry entry;
    entry.type = QStringLiteral("ZSTD_FILE");
    entry.name = payloadName(logicalName);
    entry.language = QStringLiteral("neutral");
    entry.dataSize = quint64(decoded.size());
    entry.format = ModuleFormat::ZSTD;
    entry.hierarchyPath = QStringList() << entry.name;
    entry.isEmbeddedFile = true;
    entry.content = std::make_shared<fs::MemoryStore>(std::move(decoded));
    module->resources_.push_back(std::move(entry));
    return module;
}

} // namespace peare
